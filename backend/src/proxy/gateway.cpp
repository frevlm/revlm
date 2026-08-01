#include "proxy/gateway.hpp"

#include "auth/security.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy/upstream.hpp"
#include "proxy/anthropics_messages.hpp"
#include "proxy/openai_chat.hpp"
#include "proxy/openai_responses.hpp"
#include "request/request.hpp"
#include "store/balance_ledger.hpp"
#include "store/batch_writer.hpp"
#include "store/snapshot.hpp"
#include "users/users.hpp"
#include "util/json.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace revlm
{

void write_upstream(ResponseSink &res, int status, std::string body, const std::vector<UpstreamHeader> &headers)
{
    res.set_status(status);
    res.set_reason((status >= 200 && status < 300) ? "OK" : "Upstream");
    std::string content_type = "application/json; charset=utf-8";
    for (const UpstreamHeader &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length") {
            continue;
        }
        if (lower == "content-type") {
            content_type = header.value;
            continue;
        }
        res.set_header(header.name, header.value);
    }
    res.set_content(std::move(body), content_type);
}

json headers_to_json(const std::vector<UpstreamHeader> &headers)
{
    json out;
    for (const UpstreamHeader &header : headers) {
        out[header.name] = header.value;
    }
    return out;
}

std::vector<UpstreamHeader> headers_from_json(const json &header_obj)
{
    std::vector<UpstreamHeader> out;
    if (!header_obj.is_object()) {
        return out;
    }
    for (const auto &key : header_obj.keys()) {
        const auto value = header_obj[key].as_string();
        if (!value.has_value()) {
            continue;
        }
        out.push_back({ key, *value });
    }
    return out;
}

json make_proxy_result(int status, std::string body, const std::vector<UpstreamHeader> &headers)
{
    return json({ { "status", status }, { "header", headers_to_json(headers) }, { "body", std::move(body) } });
}

json make_proxy_error(int status, json error_body)
{
    return make_proxy_result(status, serialize(error_body), { { "Content-Type", "application/json; charset=utf-8" } });
}

void write_proxy_result(ResponseSink &res, const json &result)
{
    const int status = static_cast<int>(result["status"].as_int64().value_or(500));
    const std::string body = result["body"].as_string().value_or("");
    write_upstream(res, status, body, headers_from_json(result["header"]));
}

std::string upstream_response_id_from_headers(const std::vector<UpstreamHeader> &headers)
{
    std::string fallback;
    for (const UpstreamHeader &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        const std::string value = trim_ascii(header.value);
        if (value.empty())
            continue;
        if (lower == "x-request-id")
            return value;
        if (fallback.empty() && lower == "request-id")
            fallback = value;
    }
    return fallback;
}

void assign_request_correlation(ProxyRequest &pr, std::string_view response_id)
{
    if (!response_id.empty())
        pr.upstream.response_id = std::string{ response_id };
}

void set_stream_correlation_headers(ResponseSink &res, std::string_view response_id)
{
    if (!response_id.empty())
        res.set_header("X-Response-Id", std::string{ response_id });
}

std::vector<UpstreamHeader> merge_correlation_headers(const std::vector<UpstreamHeader> &upstream_headers,
                                                      std::string_view response_id)
{
    std::vector<UpstreamHeader> headers;
    headers.reserve(upstream_headers.size() + 2);
    for (const UpstreamHeader &header : upstream_headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "x-request-id" || lower == "x-response-id")
            continue;
        headers.push_back({ header.name, header.value });
    }
    if (!response_id.empty())
        headers.push_back({ "X-Response-Id", std::string{ response_id } });
    return headers;
}

std::optional<json> paygo_balance_gate(long long user_id)
{
    if (balance_ledger().has_balance(user_id))
        return std::nullopt;
    return json({ { "error", json({ { "message", "insufficient balance" } }) } });
}

bool commit_proxy_usage(ProxyRequest &pr)
{
    if (pr.auth.user_id <= 0)
        return false;
    if (pr.auth.token_id <= 0)
        return false;
    if (pr.upstream.channel_id <= 0)
        return false;

    const double usd = compute_usd(pr);
    const int64_t usd_micro = static_cast<int64_t>(std::round(usd * 1000000.0));

    // Deduct from in-memory ledger. May go slightly negative due to
    // concurrent requests from the same user — acceptable bounded overdraft
    // since the gate already verified positive balance.
    (void)balance_ledger().deduct(pr.auth.user_id, usd_micro);

    // Defer the DB write to the background batch writer.
    batch_writer().enqueue(pr.auth.user_id, pr.auth.token_id, pr.upstream.channel_id, pr.request_id,
                           pr.upstream.response_id, pr.http.path, pr.http.method, pr.usage.input_tokens,
                           pr.usage.output_tokens, pr.usage.cache_read_tokens, pr.usage.cache_creation_1h_tokens,
                           pr.usage.cache_creation_5m_tokens, pr.upstream.tier_multiplier, pr.upstream.service_tier,
                           pr.upstream.channel_multiplier, pr.upstream.status_code, pr.upstream.latency_ms,
                           pr.upstream.first_token_latency_ms, pr.is_stream, pr.upstream.model_name, pr.error_class,
                           pr.error_message, usd_micro, pr.time);
    return true;
}

ScheduledUpstreamExecution execute_scheduled_upstream(long long channel_id, UpstreamRequest downstream)
{
    UpstreamExecutor executor;
    try {
        const int timeout_ms = config().proxy_upstream_timeout_seconds * 1000;
        const auto channel = ChannelStore::instance().find_channel(channel_id);
        if (!channel.has_value()) {
            throw std::runtime_error("channel not found");
        }
        const bool allow_private_target = upstream_channel_allows_private_target(channel->base_url);
        UpstreamExecutionResult executed = execute_with_default_transport(executor, channel_id, std::move(downstream),
                                                                          timeout_ms, allow_private_target);
        return ScheduledUpstreamExecution{
            .result = std::move(executed),
            .transport_error = std::nullopt,
        };
    } catch (const std::invalid_argument &) {
        return ScheduledUpstreamExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "parse",
                    .message = "upstream URL is invalid",
                },
        };
    } catch (const std::exception &) {
        return ScheduledUpstreamExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "connect",
                    .message = "upstream connect failed",
                },
        };
    }
}

ScheduledUpstreamStreamExecution open_scheduled_upstream_stream(long long channel_id, UpstreamRequest downstream)
{
    UpstreamExecutor executor;
    try {
        const int timeout_ms = config().proxy_upstream_timeout_seconds * 1000;
        const auto channel = ChannelStore::instance().find_channel(channel_id);
        if (!channel.has_value())
            throw std::runtime_error("channel not found");
        const bool allow_private_target = upstream_channel_allows_private_target(channel->base_url);
        const UpstreamPreparedRequest prepared =
            executor.prepare(channel_id, std::move(downstream), !allow_private_target);
        UpstreamStreamResponse upstream =
            default_upstream_http_stream_transport(prepared, timeout_ms, allow_private_target);
        return ScheduledUpstreamStreamExecution{
            .result = std::move(upstream),
            .transport_error = std::nullopt,
        };
    } catch (const std::invalid_argument &) {
        return ScheduledUpstreamStreamExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "parse",
                    .message = "upstream URL is invalid",
                },
        };
    } catch (const std::exception &err) {
        std::fprintf(stderr, "open_scheduled_upstream_stream failed: %s\n", err.what());
        return ScheduledUpstreamStreamExecution{
            .result = std::nullopt,
            .transport_error =
                GatewayAttemptTransportError{
                    .stage = "connect",
                    .message = "upstream connect failed",
                },
        };
    }
}

std::string remove_json_field(std::string_view json_text, std::string_view field_name)
{
    auto value = json::parse(json_text);
    if (!value || !value->is_object())
        return std::string{ json_text };
    value->erase(field_name);
    return value->dump();
}

UpstreamRequest build_proxy_upstream_request(const ProxyRequest &pr, std::string_view path)
{
    const std::string &client_ip = pr.http.client_ip;

    auto header_string = [&pr](std::string_view wanted_lower) -> std::string {
        for (const auto &kv : pr.http.headers) {
            if (lowercase_ascii(kv.first) == wanted_lower) {
                return kv.second;
            }
        }
        return {};
    };

    const std::string request_id = pr.request_id;

    std::string original_host = header_string("host");
    std::string forwarded_proto = "http";
    if (is_trusted_proxy_ipv4(client_ip, default_trusted_proxies())) {
        if (const auto host = trusted_forwarded_host(header_string("x-forwarded-host")); host.has_value()) {
            original_host = *host;
        }
        if (const auto proto = trusted_forwarded_proto(header_string("x-forwarded-proto")); proto.has_value()) {
            forwarded_proto = *proto;
        }
    }

    std::vector<UpstreamHeader> headers;
    headers.push_back({ "X-Request-Id", request_id });
    headers.push_back({ "X-Forwarded-Proto", forwarded_proto });
    if (!original_host.empty()) {
        headers.push_back({ "X-Forwarded-Host", original_host });
    }
    if (!client_ip.empty()) {
        headers.push_back({ "X-Forwarded-For", client_ip });
    }
    for (const auto &kv : pr.http.headers) {
        const std::string lower = lowercase_ascii(kv.first);
        if (is_hop_by_hop_header(kv.first) || lower == "host" || lower == "connection" || lower == "content-length" ||
            lower == "x-request-id" || lower == "x-forwarded-for" || lower == "x-forwarded-host" ||
            lower == "x-forwarded-proto") {
            continue;
        }
        if (lower == "authorization" || lower == "x-api-key") {
            std::fprintf(stderr, "WARNING: build_proxy_upstream_request found sensitive header '%.*s' - stripping\n",
                         static_cast<int>(kv.first.size()), kv.first.data());
            continue;
        }
        headers.push_back({ kv.first, kv.second });
    }

    UpstreamRequest downstream;
    downstream.method = "POST";
    downstream.path = std::string{ path };
    downstream.body = pr.http.body;
    downstream.content_length = pr.http.content_length;
    downstream.body_source = pr.http.body_source;
    downstream.headers = std::move(headers);
    return downstream;
}

namespace
{

bool send_all_fd(int fd, std::string_view data, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket buffer full (slow client): wait for writability, bounded
            // by the deadline so a stuck client cannot hold a pump thread
            // forever.
            for (;;) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                pollfd pfd{};
                pfd.fd = fd;
                pfd.events = POLLOUT;
                const int rc = ::poll(&pfd, 1, 50);
                if (rc > 0)
                    break;
                if (rc < 0 && errno != EINTR)
                    return false;
            }
            continue;
        }
        return false; // closed / error
    }
    return true;
}

} // namespace

ClientWriter client_writer_from_fd(int fd, std::shared_ptr<std::atomic_bool> response_started)
{
    return [fd, response_started = std::move(response_started)](std::string_view data) {
        const std::chrono::milliseconds timeout(std::max(1, config().proxy_client_write_timeout_seconds) * 1000);
        const bool sent = send_all_fd(fd, data, timeout);
        if (sent && response_started)
            response_started->store(true, std::memory_order_release);
        return sent;
    };
}

bool is_sse_content_type(std::string_view content_type)
{
    const std::string normalized = lowercase_ascii(content_type);
    return normalized.find("text/event-stream") != std::string::npos;
}

std::string read_remaining_stream(const UpstreamReadHandle &stream)
{
    if (!stream.read) {
        return {};
    }
    std::string out;
    char buffer[8192];
    for (;;) {
        const ssize_t n = stream.read(buffer, sizeof(buffer), 60000);
        if (n <= 0) {
            break;
        }
        out.append(buffer, static_cast<size_t>(n));
    }
    return out;
}

std::string drain_upstream_stream_body(UpstreamStreamResponse &upstream)
{
    std::string out = std::move(upstream.initial_body);
    out += read_remaining_stream(upstream.stream);
    if (upstream.stream.close) {
        upstream.stream.close();
    }
    return out;
}

std::string format_upstream_proxy_response_headers(int status_code, const std::vector<UpstreamHeader> &headers,
                                                   size_t body_size)
{
    std::string text = "HTTP/1.1 " + std::to_string(status_code);
    text += status_code >= 200 && status_code < 300 ? " OK\r\n" : " Upstream\r\n";
    for (const auto &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length") {
            continue;
        }
        text += header.name + ": " + header.value + "\r\n";
    }
    text += "Content-Length: " + std::to_string(body_size) + "\r\n";
    text += "Connection: close\r\n\r\n";
    return text;
}

std::string build_synthetic_stream_response_head(int status, std::string_view content_type,
                                                 const std::vector<UpstreamHeader> &headers)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << status << (status >= 200 && status < 300 ? " OK" : " Bad Gateway") << "\r\n"
        << "Content-Type: " << (content_type.empty() ? "text/event-stream; charset=utf-8" : content_type) << "\r\n";
    for (const UpstreamHeader &header : headers) {
        out << header.name << ": " << header.value << "\r\n";
    }
    out << "Connection: close\r\n\r\n";
    return out.str();
}

namespace
{

constexpr size_t kMaxSseLineBytes = 1024 * 1024; // 1 MiB
constexpr size_t kMaxSseEventBytes = 2048 * 1024; // 2 MiB
constexpr size_t kFlushBytes = 1024;
constexpr size_t kLineChunkBytes = 8192; // line buffer block size (see ChainedLine)

/// Growable line buffer built from fixed 8 KiB blocks instead of a single
/// std::string, so a long line (e.g. a 40 KiB base64 signature_delta) costs
/// exactly ⌈len/8K⌉ blocks — no 2× doubling slack from libc++ reallocation.
/// Only the completed line materialises as a std::string (one copy), and only
/// at the size it really needs.
class ChainedLine {
public:
    void append(std::string_view bytes)
    {
        size_t pos = 0;
        while (pos < bytes.size()) {
            if (tail_free_ == 0)
                push_block();
            const size_t n = std::min<size_t>(tail_free_, bytes.size() - pos);
            std::memcpy(tail_ + kLineChunkBytes - tail_free_, bytes.data() + pos, n);
            tail_free_ -= n;
            pos += n;
        }
        size_ += bytes.size();
    }

    /// Copy the accumulated line into a string and reset.  `extra` reserves
    /// headroom for the append that immediately follows (used for the
    /// `\r\n`-free raw event reconstruction).
    std::string take(size_t extra)
    {
        std::string out;
        out.reserve(size_ + extra);
        out.resize(size_);
        char *dst = out.data();
        size_t written = 0;
        for (const std::unique_ptr<char[]> &block : blocks_) {
            const size_t n = std::min<size_t>(kLineChunkBytes, size_ - written);
            std::memcpy(dst, block.get(), n);
            dst += n;
            written += n;
            if (written >= size_)
                break;
        }
        clear();
        return out;
    }

    size_t size() const
    {
        return size_;
    }
    bool empty() const
    {
        return size_ == 0;
    }
    void clear()
    {
        blocks_.clear();
        tail_ = nullptr;
        tail_free_ = 0;
        size_ = 0;
    }

private:
    void push_block()
    {
        blocks_.push_back(std::make_unique<char[]>(kLineChunkBytes));
        tail_ = blocks_.back().get();
        tail_free_ = kLineChunkBytes;
    }

    std::deque<std::unique_ptr<char[]>> blocks_;
    char *tail_ = nullptr;
    size_t tail_free_ = 0;
    size_t size_ = 0;
};

struct SseEvent {
    std::string data;
    std::string raw_event;
    bool done = false;
};

class SseReader {
public:
    /// Scan a response chunk and produce:
    ///   - `forward_spans`: complete events fully contained in this chunk
    ///     that need no parsing — forwarded verbatim as string_views into
    ///     the chunk (zero-copy; design doc §9 — the vast majority of SSE
    ///     events are pure deltas that must never touch line_/raw_event_).
    ///   - `events`: everything else (cross-chunk events, CRLF input,
    ///     events mentioning usage/model/message_stop/[DONE]) accumulated
    ///     into data_/raw_event_ as before.
    /// The views in `forward_spans` reference `chunk` and are only valid
    /// until the next call.
    bool consume(std::string_view chunk, std::vector<std::string_view> &forward_spans, std::vector<SseEvent> &events)
    {
        if (!ok_) {
            return false;
        }

        size_t pos = 0;

        // 1) Finish a line that straddled the previous chunk boundary.
        if (!line_pending_.empty()) {
            const size_t nl = chunk.find('\n');
            if (nl == std::string_view::npos) {
                if (line_pending_.size() + chunk.size() > kMaxSseLineBytes) {
                    ok_ = false;
                    return false;
                }
                line_pending_.append(chunk);
                return ok_;
            }
            std::string_view tail = chunk.substr(0, nl);
            while (!tail.empty() && tail.back() == '\r')
                tail.remove_suffix(1);
            line_pending_.append(tail);
            std::string line = line_pending_.take(0);
            event_bytes_ += line.size() + 1;
            if (event_bytes_ > kMaxSseEventBytes) {
                ok_ = false;
                return false;
            }
            if (!push_line(line, events)) {
                return false;
            }
            pos = nl + 1;
            block_event_start_ = std::string_view::npos; // any straddling line kills block refs
        }

        // 2) In-chunk fast scan.
        while (pos < chunk.size()) {
            const size_t nl = chunk.find('\n', pos);
            if (nl == std::string_view::npos) {
                // Trailing line without a newline: stash it.  If an event
                // started in this chunk, its bytes must move to the
                // accumulation buffers before the next chunk continues it.
                const std::string_view rest = chunk.substr(pos);
                if (event_open_ && block_event_start_ != std::string_view::npos) {
                    const std::string_view seen = chunk.substr(block_event_start_, pos - block_event_start_);
                    event_bytes_ += seen.size();
                    if (event_bytes_ > kMaxSseEventBytes) {
                        ok_ = false;
                        return false;
                    }
                    flush_block_lines(seen, events);
                    block_event_start_ = std::string_view::npos;
                }
                if (rest.size() > kMaxSseLineBytes) {
                    ok_ = false;
                    return false;
                }
                line_pending_.clear();
                line_pending_.append(rest);
                break;
            }

            std::string_view line = chunk.substr(pos, nl - pos);
            while (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            if (line.empty()) {
                // Blank line = end of the current event.
                if (event_open_) {
                    if (block_event_start_ != std::string_view::npos) {
                        const std::string_view ev = chunk.substr(block_event_start_, nl + 1 - block_event_start_);
                        event_bytes_ += ev.size();
                        if (event_bytes_ > kMaxSseEventBytes) {
                            ok_ = false;
                            return false;
                        }
                        dispatch_block_event(ev, forward_spans, events);
                        block_event_start_ = std::string_view::npos;
                        event_open_ = false;
                        event_bytes_ = 0;
                    } else {
                        // Cross-chunk event complete.
                        if (!push_line({}, events)) {
                            return false;
                        }
                    }
                }
            } else {
                if (!event_open_) {
                    event_open_ = true;
                    block_event_start_ = pos;
                } else if (block_event_start_ == std::string_view::npos) {
                    // Continuation of an event that straddled chunk
                    // boundaries: the block fast-path is gone, so this line
                    // must accumulate line-by-line.
                    event_bytes_ += line.size() + 1;
                    if (event_bytes_ > kMaxSseEventBytes) {
                        ok_ = false;
                        return false;
                    }
                    if (!push_line(line, events)) {
                        return false;
                    }
                }
            }
            pos = nl + 1;
        }
        return ok_;
    }

    bool finish()
    {
        if (!ok_) {
            return false;
        }
        if (event_open_) {
            data_.clear();
            raw_event_.clear();
            event_open_ = false;
        }
        line_pending_.clear();
        return true;
    }

    std::string drain_partial()
    {
        std::string result = std::move(raw_event_);
        raw_event_.clear();
        if (!line_pending_.empty()) {
            // Trailing line without a terminating newline (e.g. a non-SSE JSON
            // response that ends at EOF).  Flush it as the final event so the
            // body isn't silently dropped.
            if (!result.empty()) {
                result.push_back('\n');
            }
            result += line_pending_.take(0);
        }
        if (event_open_) {
            data_.clear();
            event_open_ = false;
        }
        return result;
    }

private:
    /// A complete event that lives wholly inside the current chunk: forward
    /// it as a span when it is pure (no CR, no usage/model/message_stop/
    /// [DONE]), otherwise assemble it through push_line so parsing and
    /// done-detection stay identical.
    void dispatch_block_event(std::string_view ev, std::vector<std::string_view> &forward_spans,
                              std::vector<SseEvent> &events)
    {
        const bool clean = ev.find('\r') == std::string_view::npos;
        const bool special =
            ev.find("\"usage\"") != std::string_view::npos || ev.find("\"model\"") != std::string_view::npos ||
            ev.find("message_stop") != std::string_view::npos ||
            ev.find("response.completed") != std::string_view::npos || ev.find("[DONE]") != std::string_view::npos;
        if (clean && !special) {
            forward_spans.push_back(ev);
            return;
        }
        flush_block_lines(ev, events);
    }

    /// Split an in-chunk event into lines and feed each through push_line
    /// (accumulation path — data_/raw_event_/done semantics unchanged).
    void flush_block_lines(std::string_view ev, std::vector<SseEvent> &events)
    {
        size_t p = 0;
        while (p < ev.size()) {
            const size_t nl = ev.find('\n', p);
            const size_t end = nl == std::string_view::npos ? ev.size() : nl;
            std::string_view line = ev.substr(p, end - p);
            while (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (!push_line(line, events)) {
                return;
            }
            if (nl == std::string_view::npos) {
                break;
            }
            p = nl + 1;
        }
    }

    bool push_line(std::string_view line, std::vector<SseEvent> &out)
    {
        if (line.empty()) {
            if (event_open_) {
                raw_event_ += '\n';
                const bool done = trim_ascii(data_) == "[DONE]";
                out.push_back(SseEvent{ std::move(data_), std::move(raw_event_), done });
                raw_event_.clear();
                data_.clear();
                event_open_ = false;
            }
            return true;
        }

        event_open_ = true;
        // Reserve exactly what the line needs (plus a few bytes for the
        // trailing '\n'): raw_event_ is built per event and cleared after,
        // so a 40 KiB line never grows data_ past its content.
        raw_event_.reserve(raw_event_.size() + line.size() + 1);
        raw_event_.append(line.data(), line.size());
        raw_event_ += '\n';
        if (line[0] == ':') {
            return true;
        }
        const size_t colon = line.find(':');
        std::string_view field = line;
        std::string_view value;
        if (colon != std::string_view::npos) {
            field = line.substr(0, colon);
            value = line.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') {
                value.remove_prefix(1);
            }
        }
        if (field == "data") {
            if (!data_.empty()) {
                data_.push_back('\n');
            }
            // data_ mirrors the line's payload exactly: reserve what the
            // value adds (plus the join newline) so a long line allocates
            // once and never doubles past its content.
            data_.reserve(data_.size() + value.size() + 1);
            data_.append(value.data(), value.size());
        }
        return true;
    }

    ChainedLine line_pending_; // trailing line across chunk boundaries (8 KiB blocks)
    std::string data_;
    std::string raw_event_;
    size_t event_bytes_ = 0;
    size_t block_event_start_ = std::string_view::npos; // in-chunk event origin
    bool event_open_ = false;
    bool ok_ = true;
};

bool contains_usage_object(const json &value)
{
    if (value.is_object()) {
        if (value["usage"].is_object()) {
            return true;
        }
        for (const auto &key : value.keys()) {
            if (contains_usage_object(value[key])) {
                return true;
            }
        }
        return false;
    }
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (contains_usage_object(value[i])) {
                return true;
            }
        }
    }
    return false;
}

std::optional<std::string> find_first_model(const json &value)
{
    if (value.is_object()) {
        for (const auto &key : value.keys()) {
            if (key == "model") {
                if (const auto model = value[key].as_string(); model.has_value() && !model->empty()) {
                    return *model;
                }
            }
            if (const auto nested = find_first_model(value[key])) {
                return nested;
            }
        }
        return std::nullopt;
    }
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (const auto nested = find_first_model(value[i])) {
                return nested;
            }
        }
    }
    return std::nullopt;
}

void handle_sse_event(const SseEvent &event, const std::chrono::steady_clock::time_point &started_at,
                      GatewayStreamPump &pump, Gateway &gateway)
{
    if (event.done) {
        pump.completed = true;
        return;
    }
    if (pump.first_token_latency_ms == 0) {
        pump.first_token_latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at)
                .count());
    }
    // trim_ascii returns a std::string — hold it, do not bind a view to the
    // temporary (the old binding was dangling and reads were garbage).
    const std::string data = trim_ascii(event.data);

    // Fast path: the vast majority of SSE events are pure deltas with no
    // usage/model — forward them without json::parse (design doc §9).  Only
    // events that mention "usage" or "model" are worth parsing.
    if (data.find("\"usage\"") == std::string_view::npos && data.find("\"model\"") == std::string_view::npos) {
        if (data.find("message_stop") != std::string_view::npos ||
            data.find("response.completed") != std::string_view::npos) {
            pump.completed = true;
        }
        return;
    }

    auto doc = json::parse(data);
    if (!doc || !doc->is_object()) {
        return;
    }
    const json &root = *doc;
    if (const auto type_str = root["type"].as_string(); type_str.has_value()) {
        if (*type_str == "message_stop" || *type_str == "response.completed") {
            pump.completed = true;
        }
    }
    if (const auto model = find_first_model(root)) {
        pump.model = *model;
    }
    if (contains_usage_object(root)) {
        // finalize must never break the stream: a provider sending a sparse
        // usage object is a billing-data problem, not a connection problem.
        try {
            gateway.finalize(*doc);
        } catch (const std::exception &err) {
            std::fprintf(stderr, "finalize failed (billing skipped): %s\n", err.what());
            return;
        }
        pump.saw_usage = true;
    }
}

} // namespace

UpstreamRequest Gateway::make_upstream(bool stream) const
{
    (void)stream;
    return build_proxy_upstream_request(request, upstream_path());
}

std::string extract_requested_model(std::string_view body)
{
    if (body.empty()) {
        return {};
    }
    // Full JSON body (httplib path) — exact top-level "model" field.
    if (auto doc = json::parse(body); doc && doc->is_object()) {
        if (const auto model = (*doc)["model"].as_string(); model.has_value() && !model->empty()) {
            return *model;
        }
    }
    // Truncated sliding-window prefix (Beast path): scan for the "model" key
    // and its string value.  The caller validates the result against the
    // channel model table, so a false positive inside message content is
    // rejected rather than billed.
    size_t pos = 0;
    while ((pos = body.find("\"model\"", pos)) != std::string_view::npos) {
        size_t i = pos + 7; // strlen("\"model\"")
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t'))
            ++i;
        if (i >= body.size() || body[i] != ':') {
            pos += 7;
            continue;
        }
        ++i;
        while (i < body.size() && (body[i] == ' ' || body[i] == '\t'))
            ++i;
        if (i >= body.size() || body[i] != '"') {
            pos += 7;
            continue;
        }
        ++i;
        std::string value;
        bool closed = false;
        while (i < body.size()) {
            const char ch = body[i++];
            if (ch == '\\') {
                if (i < body.size())
                    value.push_back(body[i++]);
                continue;
            }
            if (ch == '"') {
                closed = true;
                break;
            }
            value.push_back(ch);
        }
        if (closed && !value.empty())
            return value;
        pos += 7;
    }
    return {};
}

/// Base success-path pricing, as a free function so it can be invoked from
/// provider callbacks that outlive the Gateway object (the on_complete lambdas
/// registered on httplib/Beast sinks run after the temporary Gateway is gone).
void apply_success_pricing(ProxyRequest &pr, const Channel &channel)
{
    pr.upstream.channel_multiplier = channel.price_multiplier;

    // Billing uses the client-REQUESTED model, not the upstream response
    // model: the upstream may rewrite the model (e.g. cc-switch maps a front
    // model to a backend model), and the channel price table only knows the
    // front model.  The requested model is extracted from the request body
    // (full body on the httplib path, sliding-window peek on the Beast path)
    // and validated against the channel table before overriding.
    std::string body_copy;
    std::string_view source = pr.http.body;
    if (pr.http.body_source && pr.http.body_source->peek) {
        body_copy = pr.http.body_source->peek();
        source = body_copy;
    }
    std::string requested = extract_requested_model(source);
    if (!requested.empty() && channel.find_model(requested)) {
        pr.upstream.model_name = std::move(requested);
    }

    if (const Model *model = channel.find_model(pr.upstream.model_name)) {
        fill_pricing_from_model(pr.upstream.pricing, *model);
    }
}

void Gateway::fill_success_pricing(ProxyRequest &pr, const Channel &channel)
{
    apply_success_pricing(pr, channel);
}

bool Gateway::should_bill_non_stream() const
{
    return true;
}

bool Gateway::prepare(ResponseSink &res)
{
    (void)res;
    return true;
}

std::string_view Gateway::no_available_channel_message() const
{
    return "no available channel";
}

std::optional<ChannelGroup> Gateway::load_channel_group() const
{
    const long long channel_group_id = request.auth.channel_group_id;
    if (channel_group_id <= 0) {
        return std::nullopt;
    }
    // Snapshot-first fast path (lock-free read, channels already filled).
    if (auto snapshot = snapshot_acquire()) {
        auto it = snapshot->groups.find(channel_group_id);
        if (it != snapshot->groups.end()) {
            ChannelGroup group = it->second;
            if (group.id <= 0 || !group.status || group.channels.empty()) {
                return std::nullopt;
            }
            if (group.pointer < 0 || group.pointer >= static_cast<int>(group.channels.size())) {
                group.pointer = 0;
            }
            return group;
        }
    }
    // DB fallback.
    ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
    if (group.id <= 0 || !group.status || group.channels.empty()) {
        return std::nullopt;
    }
    if (group.pointer < 0 || group.pointer >= static_cast<int>(group.channels.size())) {
        group.pointer = 0;
    }
    return group;
}

json Gateway::run()
{
    auto group = load_channel_group();
    if (!group.has_value()) {
        return make_proxy_error(400, json{ { "error", json{ { "message", "channel group unavailable" } } } });
    }

    Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
    if (!channel_ok(channel)) {
        return make_proxy_error(
            400, json{ { "error", json{ { "message", std::string{ no_available_channel_message() } } } } });
    }

    ScheduledUpstreamExecution executed = execute_scheduled_upstream(channel.id, make_upstream(false));
    if (executed.result.has_value() && executed.result->response.status_code < 400) {
        UpstreamResponse &resp = executed.result->response;
        const std::string response_id = upstream_response_id_from_headers(resp.headers);
        request.upstream.channel_id = channel.id;
        // model_name stays empty here — fill_success_pricing captures the
        // client-requested model (the upstream may rewrite the response model).
        fill_success_pricing(request, channel);
        request.upstream.status_code = resp.status_code;
        request.upstream.response_id = response_id;
        assign_request_correlation(request, response_id);
        if (const auto response_tier = parse_json_string_field(resp.body, "service_tier"); response_tier.has_value()) {
            request.upstream.service_tier = *response_tier;
        }
        parse_billing_request_from_body(request, kind(), resp.body);
        request.http.body.clear();
        request.http.body.shrink_to_fit();
        return make_proxy_result(resp.status_code, std::move(resp.body),
                                 merge_correlation_headers(resp.headers, response_id));
    }

    if (executed.result.has_value()) {
        UpstreamResponse &resp = executed.result->response;
        const std::string response_id = upstream_response_id_from_headers(resp.headers);
        request.upstream.channel_id = channel.id;
        request.upstream.status_code = resp.status_code;
        return make_proxy_result(resp.status_code, std::move(resp.body),
                                 merge_correlation_headers(resp.headers, response_id));
    }

    return make_proxy_error(400,
                            json{ { "error", json{ { "message", std::string{ no_available_channel_message() } } } } });
}

void Gateway::run_stream(ResponseSink &res, const std::function<void(ProxyRequest &)> &on_usage)
{
    auto group = load_channel_group();
    if (!group.has_value()) {
        write_proxy_result(
            res, make_proxy_error(400, json{ { "error", json{ { "message", "channel group unavailable" } } } }));
        return;
    }

    Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
    if (!channel_ok(channel)) {
        write_proxy_result(
            res, make_proxy_error(
                     400, json{ { "error", json{ { "message", std::string{ no_available_channel_message() } } } } }));
        return;
    }

    // Billing model: capture the client-requested model while the request
    // body is still in memory — the body is released before the SSE pump,
    // which outlives this call, and the upstream may rewrite the response
    // model (e.g. cc-switch maps a front model to a backend model).
    if (request.upstream.model_name.empty()) {
        const std::string requested = extract_requested_model(request.http.body);
        if (!requested.empty() && channel.find_model(requested)) {
            request.upstream.model_name = requested;
        }
    }

    ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(channel.id, make_upstream(true));
    if (executed.result.has_value() && executed.result->status_code < 400) {
        UpstreamStreamResponse upstream = std::move(*executed.result);
        const int status = upstream.status_code;
        const std::string response_id = upstream_response_id_from_headers(upstream.headers);
        const long long channel_id = channel.id;
        const double route_mult = channel.price_multiplier;
        request.upstream.channel_id = channel_id;
        request.upstream.status_code = status;
        request.upstream.channel_multiplier = route_mult;
        request.is_stream = true;
        request.upstream.response_id = response_id;
        assign_request_correlation(request, response_id);

        request.http.body.clear();
        request.http.body.shrink_to_fit();
        const GatewayStreamKind stream_kind = kind();
        // Extract headers into a standalone vector FIRST: apply_upstream_gateway_stream
        // receives `upstream` by move, and parameter-initialization order is
        // unspecified — a `upstream.headers` reference argument could alias the
        // moved-from vector (observed empty headers).
        std::vector<UpstreamHeader> stream_headers = std::move(upstream.headers);
        // Must run BEFORE apply_upstream_gateway_stream: set_chunked_provider
        // writes the response head synchronously, so any header set after the
        // call never reaches the wire.
        set_stream_correlation_headers(res, response_id);
        apply_upstream_gateway_stream(
            res, status, std::move(stream_headers), std::move(upstream), std::move(request),
            [stream_kind](ProxyRequest &u) -> std::unique_ptr<Gateway> { return make_gateway(stream_kind, u); },
            [status, on_usage, channel_id, route_mult](ProxyRequest &u, const GatewayStreamResult &result) {
                const GatewayStreamPump &pump = result.pump;
                const bool success = status < 400 && pump.completed && !pump.upstream_error && !pump.idle_timeout;
                if (!on_usage || !success || !pump.saw_usage) {
                    return;
                }
                if (const auto channel = ChannelStore::instance().find_channel(channel_id); channel.has_value()) {
                    // Free function — this callback runs after the Gateway
                    // temporary is destroyed, so it must not capture `this`.
                    apply_success_pricing(u, *channel);
                    u.upstream.channel_multiplier = route_mult;
                }
                u.upstream.first_token_latency_ms = pump.first_token_latency_ms;
                on_usage(u);
            });
        return;
    }

    if (!executed.result.has_value()) {
        write_upstream(res, 502, serialize(json{ { "error", json{ { "message", "proxy upstream failed" } } } }),
                       { { "Content-Type", "application/json; charset=utf-8" } });
    } else {
        const int status = executed.result->status_code;
        std::string body = drain_upstream_stream_body(*executed.result);
        request.upstream.channel_id = channel.id;
        request.upstream.status_code = status;
        write_upstream(res, status, std::move(body), merge_correlation_headers(executed.result->headers, {}));
    }
}

namespace
{

using Clock = std::chrono::steady_clock;

struct ResponseHead {
    int status = 502;
    std::string body;
    std::string content_type;
    std::string response_id;
};

struct ProxyUpstreamResponse {
    int status = 502;
    std::string body;
    std::string content_type;
    std::string response_id;
};

struct UpstreamSession {
    ResponseHead head;
    UpstreamReadHandle stream;

    void close_stream()
    {
        if (stream.close) {
            stream.close();
        }
    }

    UpstreamSession() = default;
    UpstreamSession(const UpstreamSession &) = delete;
    UpstreamSession &operator=(const UpstreamSession &) = delete;
    UpstreamSession(UpstreamSession &&other) noexcept
        : head(std::move(other.head))
        , stream(std::move(other.stream))
    {
        other.stream = {};
    }
    UpstreamSession &operator=(UpstreamSession &&other) noexcept
    {
        if (this == &other) {
            return *this;
        }
        close_stream();
        head = std::move(other.head);
        stream = std::move(other.stream);
        other.stream = {};
        return *this;
    }
    ~UpstreamSession()
    {
        close_stream();
    }
};

json gateway_json_error_body(std::string_view message)
{
    return json{ { "error", json{ { "message", std::string{ message } } } } };
}

void write_proxy_upstream_response(ResponseSink &res, const ProxyUpstreamResponse &upstream)
{
    std::vector<UpstreamHeader> headers;
    if (!upstream.response_id.empty()) {
        headers.push_back({ "X-Response-Id", upstream.response_id });
    }
    if (!upstream.content_type.empty()) {
        headers.push_back({ "Content-Type", upstream.content_type });
    }
    write_upstream(res, upstream.status, upstream.body, headers);
}

std::string content_type_from_headers(const std::vector<UpstreamHeader> &headers)
{
    for (const auto &header : headers) {
        if (lowercase_ascii(header.name) == "content-type") {
            return trim_ascii(header.value);
        }
    }
    return {};
}

ProxyUpstreamResponse perform_gateway_upstream_request(long long channel_id, UpstreamRequest downstream)
{
    const ScheduledUpstreamExecution executed = execute_scheduled_upstream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        throw std::runtime_error(executed.transport_error->message.empty() ? "upstream unavailable" :
                                                                             executed.transport_error->message);
    }
    return ProxyUpstreamResponse{
        .status = executed.result->response.status_code,
        .body = std::move(executed.result->response.body),
        .content_type = content_type_from_headers(executed.result->response.headers),
        .response_id = upstream_response_id_from_headers(executed.result->response.headers),
    };
}

UpstreamSession open_gateway_upstream_stream_session(long long channel_id, UpstreamRequest downstream)
{
    ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(channel_id, std::move(downstream));
    if (!executed.result.has_value()) {
        throw std::runtime_error(executed.transport_error->message.empty() ? "upstream unavailable" :
                                                                             executed.transport_error->message);
    }
    UpstreamStreamResponse stream_response = std::move(*executed.result);
    UpstreamSession session;
    session.head.status = stream_response.status_code;
    session.head.body = std::move(stream_response.initial_body);
    session.head.content_type = content_type_from_headers(stream_response.headers);
    session.head.response_id = upstream_response_id_from_headers(stream_response.headers);
    session.stream = std::move(stream_response.stream);
    return session;
}

void stream_gateway_session_to_httplib(ResponseSink &res, UpstreamSession session, ProxyRequest usage,
                                       GatewayStreamKind stream_kind, double route_group_multiplier,
                                       std::function<void(ProxyRequest &usage, int first_token_latency_ms)> on_complete)
{
    const int stream_status = session.head.status;
    const std::string content_type = session.head.content_type.empty() ? "text/event-stream; charset=utf-8" :
                                                                         session.head.content_type;
    res.set_status(stream_status);
    res.set_header("Content-Type", content_type);
    if (!session.head.response_id.empty()) {
        res.set_header("X-Response-Id", session.head.response_id);
    }

    struct Shared {
        UpstreamSession session;
        ProxyRequest usage;
        GatewayStreamKind stream_kind = GatewayStreamKind::openai_responses;
        double channel_multiplier = 1.0;
        std::function<void(ProxyRequest &, int)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->session = std::move(session);
    shared->usage = std::move(usage);
    shared->usage.upstream.channel_multiplier = route_group_multiplier;
    shared->stream_kind = stream_kind;
    shared->channel_multiplier = route_group_multiplier;
    shared->on_complete = std::move(on_complete);

    const int idle_timeout_ms = std::max(1000, config().proxy_upstream_idle_timeout_seconds * 1000);
    res.set_chunked_provider(content_type, [shared, idle_timeout_ms](ChunkedSink &sink) mutable {
        auto stream_gateway = make_gateway(shared->stream_kind, shared->usage);
        const GatewayStreamResult gateway_result = pump_gateway_stream(
            shared->session.stream.read, [&sink](std::string_view data) { return sink.write(data); },
            shared->session.head.body, idle_timeout_ms, *stream_gateway);
        const int session_status = shared->session.head.status;
        shared->session.close_stream();
        if (shared->on_complete && gateway_result.pump.saw_usage && session_status < 400) {
            shared->on_complete(shared->usage, gateway_result.pump.first_token_latency_ms);
        }
        sink.done();
    });
}

bool stream_gateway_session_to_client(UpstreamSession &session, const ClientWriter &write_client, ProxyRequest &usage,
                                      GatewayStreamKind stream_kind,
                                      std::optional<std::string> &upstream_response_model, long long &response_bytes,
                                      int &first_token_latency_ms, bool &had_usage_out)
{
    std::vector<UpstreamHeader> headers;
    if (!session.head.response_id.empty()) {
        headers.push_back({ "X-Response-Id", session.head.response_id });
    }
    if (!write_client(build_synthetic_stream_response_head(session.head.status, session.head.content_type, headers))) {
        return false;
    }
    auto gateway = make_gateway(stream_kind, usage);
    const int idle_timeout_ms = std::max(1000, config().proxy_upstream_idle_timeout_seconds * 1000);
    const GatewayStreamResult result =
        pump_gateway_stream(session.stream.read, write_client, session.head.body, idle_timeout_ms, *gateway);
    if (session.stream.close) {
        session.stream.close();
    }
    response_bytes = static_cast<long long>(result.pump.response_bytes);
    first_token_latency_ms = result.pump.first_token_latency_ms;
    had_usage_out = result.pump.saw_usage;
    if (result.pump.model.has_value()) {
        upstream_response_model = *result.pump.model;
    }
    return !result.pump.upstream_error;
}

} // namespace

namespace
{

void write_json_error_to_writer(const ClientWriter &write_client, int status, std::string_view message)
{
    const std::string body = serialize(json{ { "error", json{ { "message", std::string{ message } } } } });
    const std::string head = format_upstream_proxy_response_headers(
        status, { { "Content-Type", "application/json; charset=utf-8" } }, body.size());
    (void)write_client(head);
    (void)write_client(body);
}

} // namespace

void Gateway::run_stream_writer(const ClientWriter &write_client, const std::function<void(ProxyRequest &)> &on_usage)
{
    const auto started_at = Clock::now();

    auto group = load_channel_group();
    if (!group.has_value()) {
        write_json_error_to_writer(write_client, 400, "channel group unavailable");
        return;
    }

    Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
    if (!channel_ok(channel)) {
        write_json_error_to_writer(write_client, 400, no_available_channel_message());
        return;
    }

    const long long channel_id = channel.id;
    const double route_mult = channel.price_multiplier;

    ScheduledUpstreamStreamExecution executed = open_scheduled_upstream_stream(channel_id, make_upstream(true));
    if (!executed.result.has_value()) {
        const std::string detail = executed.transport_error.has_value() && !executed.transport_error->message.empty() ?
                                       executed.transport_error->message :
                                       "proxy upstream failed";
        write_json_error_to_writer(write_client, 502, detail);
        return;
    }

    UpstreamStreamResponse upstream = std::move(*executed.result);
    const int status = upstream.status_code;
    const std::string response_id = upstream_response_id_from_headers(upstream.headers);
    request.upstream.channel_id = channel_id;
    request.upstream.status_code = status;
    request.upstream.channel_multiplier = route_mult;
    request.upstream.response_id = response_id;
    request.is_stream = true;
    request.http.body.clear();
    request.http.body.shrink_to_fit();

    const std::string content_type = content_type_from_headers(upstream.headers);
    if (status >= 400 || !is_sse_content_type(content_type)) {
        std::string body = std::move(upstream.initial_body);
        body += read_remaining_stream(upstream.stream);
        if (upstream.stream.close) {
            upstream.stream.close();
        }
        std::vector<UpstreamHeader> headers;
        if (!response_id.empty()) {
            headers.push_back({ "X-Response-Id", response_id });
        }
        headers.push_back({ "Content-Type", content_type.empty() ? "application/json; charset=utf-8" : content_type });
        const std::string head = format_upstream_proxy_response_headers(status, headers, body.size());
        (void)write_client(head);
        (void)write_client(body);
        return;
    }

    // SSE: write the response head, then pump upstream events to the client.
    std::vector<UpstreamHeader> head_headers;
    if (!response_id.empty()) {
        head_headers.push_back({ "X-Response-Id", response_id });
    }
    if (!write_client(build_synthetic_stream_response_head(status, content_type, head_headers))) {
        if (upstream.stream.close) {
            upstream.stream.close();
        }
        return;
    }

    auto gateway = make_gateway(kind(), request);
    const int idle_timeout_ms = std::max(1000, config().proxy_upstream_idle_timeout_seconds * 1000);
    GatewayStreamResult result =
        pump_gateway_stream(upstream.stream.read, write_client, upstream.initial_body, idle_timeout_ms, *gateway);
    if (upstream.stream.close) {
        upstream.stream.close();
    }

    const GatewayStreamPump &pump = result.pump;
    const bool success = status < 400 && pump.completed && !pump.upstream_error && !pump.idle_timeout;
    if (!on_usage || !success || !pump.saw_usage) {
        return;
    }
    fill_success_pricing(request, channel);
    request.upstream.latency_ms =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
    request.upstream.first_token_latency_ms =
        std::min(std::max(pump.first_token_latency_ms, 0), std::max(request.upstream.latency_ms, 0));
    request.upstream.channel_multiplier = route_mult;
    on_usage(request);
}

Gateway::HandleResult Gateway::handle(ResponseSink &res)
{
    return handle(res, StreamOptions{});
}

Gateway::HandleResult Gateway::handle(ResponseSink &res, const StreamOptions &options)
{
    if (!prepare(res)) {
        return {};
    }

    const auto request_started_at = Clock::now();
    const auto elapsed_latency_ms = [&request_started_at]() {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started_at).count());
    };

    try {
        // Copy before clear so upstream still receives the original body.
        const std::string body = request.http.body;
        request.http.body.clear();
        request.http.body.shrink_to_fit();

        auto group = load_channel_group();
        if (!group.has_value()) {
            write_upstream(res, 400, serialize(gateway_json_error_body("channel group unavailable")),
                           { { "Content-Type", "application/json; charset=utf-8" } });
            return {};
        }

        ClientWriter write_client = options.write_client;
        if (!write_client && options.client_fd >= 0) {
            write_client = client_writer_from_fd(options.client_fd);
        }
        const bool stream_sink_ready = write_client || options.stream_response != nullptr;

        Channel &channel = group->channels[static_cast<size_t>(group->pointer)];
        if (!channel_ok(channel)) {
            write_upstream(res, 400, serialize(gateway_json_error_body(no_available_channel_message())),
                           { { "Content-Type", "application/json; charset=utf-8" } });
            return {};
        }

        const long long channel_id = channel.id;
        const double route_mult = channel.price_multiplier;

        // Billing model: capture the client-requested model while the local
        // `body` copy is alive — `request.http.body` is released below and
        // the upstream may rewrite the response model.
        if (request.upstream.model_name.empty()) {
            const std::string requested = extract_requested_model(body);
            if (!requested.empty() && channel.find_model(requested)) {
                request.upstream.model_name = requested;
            }
        }

        // Temporarily restore body for make_upstream (chat-style builders read request.http.body).
        request.http.body = body;
        UpstreamRequest downstream = make_upstream(stream_sink_ready);
        request.http.body.clear();
        request.http.body.shrink_to_fit();

        std::optional<UpstreamSession> stream_session;
        ProxyUpstreamResponse upstream;

        try {
            if (stream_sink_ready) {
                UpstreamSession session = open_gateway_upstream_stream_session(channel_id, std::move(downstream));
                if (session.head.status >= 400 || !is_sse_content_type(session.head.content_type)) {
                    upstream = ProxyUpstreamResponse{
                        .status = session.head.status,
                        .body = session.head.body + read_remaining_stream(session.stream),
                        .content_type = session.head.content_type,
                        .response_id = session.head.response_id,
                    };
                    request.upstream.channel_id = channel_id;
                    request.upstream.status_code = upstream.status;
                } else {
                    stream_session = std::move(session);
                }
            } else {
                upstream = perform_gateway_upstream_request(channel_id, std::move(downstream));
                request.upstream.channel_id = channel_id;
                request.upstream.status_code = upstream.status;
            }
        } catch (const std::exception &err) {
            write_upstream(res, 502, serialize(gateway_json_error_body(err.what())),
                           { { "Content-Type", "application/json; charset=utf-8" } });
            return {};
        }

        if (stream_session.has_value()) {
            UpstreamSession &session = *stream_session;
            const int stream_status = session.head.status;
            const GatewayStreamKind stream_kind = kind();
            request.upstream.channel_id = channel_id;
            request.upstream.status_code = stream_status;
            request.upstream.channel_multiplier = route_mult;
            request.is_stream = true;
            request.upstream.response_id = session.head.response_id;

            auto finish_stream_billing = [&](ProxyRequest &stream_request, int first_token_latency_ms) {
                fill_success_pricing(stream_request, channel);
                stream_request.upstream.latency_ms = elapsed_latency_ms();
                stream_request.upstream.first_token_latency_ms =
                    std::min(std::max(first_token_latency_ms, 0), std::max(stream_request.upstream.latency_ms, 0));
                stream_request.upstream.channel_multiplier = route_mult;
            };
            if (options.stream_response != nullptr) {
                ProxyRequest stream_usage = request;
                // This callback runs inside the chunked provider, which
                // httplib invokes AFTER the handler returns — the Gateway
                // temporary and handle() locals are gone.  Capture everything
                // by value and look the channel up again.
                stream_gateway_session_to_httplib(
                    *options.stream_response, std::move(session), std::move(stream_usage), stream_kind, route_mult,
                    [channel_id, route_mult, stream_status, request_started_at,
                     on_usage = options.on_usage](ProxyRequest &stream_request, int first_token_latency_ms) mutable {
                        if (stream_status >= 400 || !on_usage) {
                            return;
                        }
                        if (const auto ch = ChannelStore::instance().find_channel(channel_id); ch.has_value()) {
                            apply_success_pricing(stream_request, *ch);
                            stream_request.upstream.channel_multiplier = route_mult;
                        }
                        stream_request.upstream.latency_ms = static_cast<int>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started_at)
                                .count());
                        stream_request.upstream.first_token_latency_ms = std::min(
                            std::max(first_token_latency_ms, 0), std::max(stream_request.upstream.latency_ms, 0));
                        on_usage(stream_request);
                    });
                return HandleResult{
                    .handled_stream = true,
                    .stream_status = stream_status,
                };
            }
            std::optional<std::string> upstream_response_model;
            long long response_bytes = 0;
            int first_token_latency_ms = 0;
            bool had_usage = false;
            if (!stream_gateway_session_to_client(session, write_client, request, stream_kind, upstream_response_model,
                                                  response_bytes, first_token_latency_ms, had_usage)) {
                throw std::runtime_error("stream pump failed");
            }
            if (session.head.status < 400 && had_usage) {
                finish_stream_billing(request, first_token_latency_ms);
                (void)commit_proxy_usage(request);
            }
            return HandleResult{
                .handled_stream = true,
                .stream_status = session.head.status,
            };
        }

        if (!should_bill_non_stream()) {
            write_proxy_upstream_response(res, upstream);
            return {};
        }

        if (upstream.status >= 400) {
            write_proxy_upstream_response(res, upstream);
            return {};
        }

        request.upstream.channel_id = channel_id;
        request.upstream.status_code = upstream.status;
        request.is_stream = false;
        request.upstream.latency_ms = std::max(elapsed_latency_ms(), 0);
        request.upstream.response_id = upstream.response_id;
        request.upstream.tier_multiplier = 1.0;
        parse_billing_request_from_body(request, kind(), upstream.body);
        fill_success_pricing(request, channel);
        write_proxy_upstream_response(res, upstream);
        return {};
    } catch (const std::exception &err) {
        write_upstream(res, 502, serialize(gateway_json_error_body(err.what())),
                       { { "Content-Type", "application/json; charset=utf-8" } });
        return {};
    }
}

std::unique_ptr<Gateway> make_gateway(GatewayStreamKind kind, ProxyRequest &pr)
{
    switch (kind) {
    case GatewayStreamKind::openai_chat:
        return std::make_unique<OpenaiChatCompletion>(pr);
    case GatewayStreamKind::openai_responses:
        return std::make_unique<OpenaiResponses>(pr);
    case GatewayStreamKind::anthropics_messages:
        return std::make_unique<AnthropicsMessages>(pr);
    }
    return nullptr;
}

void parse_billing_request_from_body(ProxyRequest &pr, GatewayStreamKind kind, std::string_view body)
{
    auto gateway = make_gateway(kind, pr);
    if (gateway == nullptr) {
        return;
    }
    auto doc = json::parse(trim_ascii(body));
    if (!doc || !doc->is_object()) {
        return;
    }
    gateway->finalize(*doc);
}

GatewayStreamResult pump_gateway_stream(const std::function<ssize_t(char *, size_t, int idle_timeout_ms)> &read_chunk,
                                        const std::function<bool(std::string_view)> &write_to_client,
                                        std::string_view initial_body, int idle_timeout_ms, Gateway &gateway)
{
    GatewayStreamResult out;
    SseReader reader;
    std::vector<std::string_view> forward_spans;
    std::vector<SseEvent> events;
    events.reserve(8);
    std::string pending_send;
    pending_send.reserve(kFlushBytes);
    const auto started_at = std::chrono::steady_clock::now();

    auto ingest = [&](std::string_view bytes) -> bool {
        out.pump.response_bytes += bytes.size();
        forward_spans.clear();
        events.clear();
        if (!reader.consume(bytes, forward_spans, events)) {
            out.pump.upstream_error = true;
            return false;
        }
        // Zero-copy path (design doc §9): complete pure events are written
        // straight from the upstream buffer as string_views — no line_,
        // no raw_event_, no json::parse, no copies.
        for (const std::string_view span : forward_spans) {
            if (out.pump.client_disconnected)
                break;
            if (!pending_send.empty()) {
                if (!write_to_client(pending_send)) {
                    out.pump.client_disconnected = true;
                    break;
                }
                pending_send.clear();
            }
            if (!write_to_client(span)) {
                out.pump.client_disconnected = true;
            }
        }
        for (const SseEvent &event : events) {
            handle_sse_event(event, started_at, out.pump, gateway);
            if (!out.pump.client_disconnected && !event.raw_event.empty()) {
                const bool has_usage = event.data.find("\"usage\"") != std::string::npos;
                if (has_usage) {
                    pending_send.append(event.raw_event);
                    if (pending_send.size() >= kFlushBytes) {
                        if (!write_to_client(pending_send)) {
                            out.pump.client_disconnected = true;
                        }
                        pending_send.clear();
                    }
                } else {
                    if (!pending_send.empty()) {
                        if (!write_to_client(pending_send)) {
                            out.pump.client_disconnected = true;
                        }
                        pending_send.clear();
                    }
                    if (!write_to_client(event.raw_event)) {
                        out.pump.client_disconnected = true;
                    }
                }
            }
        }
        return true;
    };

    if (!initial_body.empty() && !ingest(initial_body)) {
        if (!pending_send.empty() && !out.pump.client_disconnected) {
            (void)write_to_client(pending_send);
        }
        return out;
    }

    char buffer[8192];
    for (;;) {
        const ssize_t n = read_chunk(buffer, sizeof(buffer), idle_timeout_ms);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            // ETIMEDOUT = no response bytes within the inter-event idle window
            // (the read itself drives curl; the idle window only starts after
            // the request body has been fully sent).
            if (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!out.pump.client_disconnected) {
                    out.pump.idle_timeout = true;
                }
            } else {
                out.pump.upstream_error = true;
            }
            break;
        }
        if (!ingest(std::string_view{ buffer, static_cast<size_t>(n) })) {
            break;
        }
        if (out.pump.client_disconnected && out.pump.completed) {
            break;
        }
    }

    {
        std::string leftover = reader.drain_partial();
        if (!leftover.empty() && !out.pump.client_disconnected) {
            pending_send.append(leftover);
        }
    }
    if (!reader.finish()) {
        out.pump.upstream_error = true;
    }
    if (!pending_send.empty() && !out.pump.client_disconnected) {
        (void)write_to_client(pending_send);
    }

    return out;
}

void apply_upstream_gateway_stream(ResponseSink &res, int status, std::vector<UpstreamHeader> headers,
                                   UpstreamStreamResponse upstream, ProxyRequest usage,
                                   std::function<std::unique_ptr<Gateway>(ProxyRequest &)> make_gateway_for_usage,
                                   std::function<void(ProxyRequest &usage, const GatewayStreamResult &)> on_complete)
{
    res.set_status(status);
    std::string content_type = "text/event-stream; charset=utf-8";
    for (const UpstreamHeader &header : headers) {
        const std::string lower = lowercase_ascii(header.name);
        if (lower == "connection" || lower == "transfer-encoding" || lower == "content-length") {
            continue;
        }
        if (lower == "content-type") {
            content_type = header.value;
            continue;
        }
        res.set_header(header.name, header.value);
    }
    struct Shared {
        UpstreamStreamResponse upstream;
        ProxyRequest usage;
        std::unique_ptr<Gateway> gateway;
        int idle_timeout_ms = 0;
        std::function<void(ProxyRequest &usage, const GatewayStreamResult &)> on_complete;
    };
    auto shared = std::make_shared<Shared>();
    shared->upstream = std::move(upstream);
    shared->usage = std::move(usage);
    shared->gateway = make_gateway_for_usage(shared->usage);
    shared->idle_timeout_ms = std::max(1000, config().proxy_upstream_idle_timeout_seconds * 1000);
    shared->on_complete = std::move(on_complete);

    res.set_chunked_provider(content_type, [shared](ChunkedSink &sink) mutable {
        try {
            GatewayStreamResult result;
            auto tracked_write = [&sink](std::string_view data) { return sink.write(data); };
            if (shared->gateway) {
                result = pump_gateway_stream(shared->upstream.stream.read, tracked_write, shared->upstream.initial_body,
                                             shared->idle_timeout_ms, *shared->gateway);
            } else {
                auto write = [&tracked_write, &result](std::string_view data) {
                    result.pump.response_bytes += data.size();
                    return tracked_write(data);
                };
                if (!shared->upstream.initial_body.empty()) {
                    (void)write(shared->upstream.initial_body);
                }
                char buffer[8192];
                for (;;) {
                    const ssize_t n = shared->upstream.stream.read(buffer, sizeof(buffer), shared->idle_timeout_ms);
                    if (n <= 0) {
                        break;
                    }
                    if (!write(std::string_view{ buffer, static_cast<size_t>(n) })) {
                        result.pump.client_disconnected = true;
                        break;
                    }
                }
            }
            if (shared->upstream.stream.close) {
                shared->upstream.stream.close();
            }
            if (shared->on_complete) {
                shared->on_complete(shared->usage, result);
            }
            sink.done();
        } catch (const std::exception &err) {
            std::cerr << "chunked stream provider failed: " << err.what() << std::endl;
            try {
                sink.done();
            } catch (...) {
            }
        } catch (...) {
            std::cerr << "chunked stream provider failed: unknown" << std::endl;
            try {
                sink.done();
            } catch (...) {
            }
        }
    });
}

} // namespace revlm
