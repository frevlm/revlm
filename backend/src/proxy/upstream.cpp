#include "proxy/upstream.hpp"

#include "auth/security.hpp"
#include "channels/channels.hpp"
#include "util/json.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>
#include "util/strings.hpp"

#include "net/curl_multi_pool.hpp"
#include <netdb.h>

namespace revlm
{
namespace
{

// Per-thread CurlMultiPool — survives across requests for connection reuse.
thread_local std::unique_ptr<CurlMultiPool> tls_pool;

CurlMultiPool &pool()
{
    if (!tls_pool)
        tls_pool = std::make_unique<CurlMultiPool>();
    return *tls_pool;
}

} // namespace
namespace
{

constexpr int k_default_upstream_timeout_ms = 30000;

bool channel_type_is_anthropic(const std::string &type)
{
    return type == "anthropic";
}

bool iequals(std::string_view left, std::string_view right)
{
    return lowercase_ascii(left) == lowercase_ascii(right);
}

std::string normalize_path(std::string_view raw)
{
    std::string path = trim_ascii(raw);
    if (path.empty()) {
        return "/";
    }
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::string join_paths(std::string_view base_path, std::string_view append_path)
{
    const std::string left = normalize_path(base_path);
    const std::string right = normalize_path(append_path);
    if (left == "/") {
        return right;
    }
    if (right == "/") {
        return left;
    }
    return left + right;
}

std::string normalize_upstream_path(const ValidatedBaseUrl &base_url, std::string_view downstream_path)
{
    std::string path = normalize_path(downstream_path);
    if (base_url.base_path == "/v1" && path.rfind("/v1/", 0) == 0) {
        path.erase(0, 3);
        if (path.empty()) {
            path = "/";
        }
    }
    return join_paths(base_url.base_path, path);
}

std::string header_value(const std::vector<UpstreamHeader> &headers, std::string_view name)
{
    for (const UpstreamHeader &header : headers) {
        if (iequals(header.name, name)) {
            return header.value;
        }
    }
    return {};
}

void set_header(std::vector<UpstreamHeader> &headers, std::string_view name, std::string value)
{
    for (UpstreamHeader &header : headers) {
        if (iequals(header.name, name)) {
            header.name = std::string{ name };
            header.value = std::move(value);
            return;
        }
    }
    headers.push_back({ std::string{ name }, std::move(value) });
}

void erase_header(std::vector<UpstreamHeader> &headers, std::string_view name)
{
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [&](const UpstreamHeader &header) { return iequals(header.name, name); }),
                  headers.end());
}

} // namespace

bool is_hop_by_hop_header(std::string_view name)
{
    const std::string lower = lowercase_ascii(name);
    return lower == "host" || lower == "content-length" || lower == "cookie" || lower == "connection" ||
           lower == "proxy-connection" || lower == "keep-alive" || lower == "proxy-authenticate" ||
           lower == "proxy-authorization" || lower == "te" || lower == "trailer" || lower == "transfer-encoding" ||
           lower == "upgrade" || lower == "x-forwarded-for" || lower == "x-forwarded-host" ||
           lower == "x-forwarded-proto" || lower == "x-revlm-remote-ip" || lower == "x-revlm-client-ip";
}

namespace
{

std::vector<UpstreamHeader> copy_headers(const std::vector<UpstreamHeader> &src)
{
    std::vector<UpstreamHeader> out;
    out.reserve(src.size());
    for (const UpstreamHeader &header : src) {
        if (is_hop_by_hop_header(header.name)) {
            continue;
        }
        out.push_back(header);
    }
    return out;
}

} // namespace

std::string build_upstream_url(const ValidatedBaseUrl &base_url, std::string_view downstream_path,
                               std::string_view query)
{
    std::string path = normalize_upstream_path(base_url, downstream_path);
    std::string url = base_url.scheme + "://" + base_url.host;
    if (!base_url.port.empty() && base_url.port != (base_url.scheme == "https" ? "443" : "80")) {
        url += ":" + base_url.port;
    }
    url += path;
    if (!trim_ascii(query).empty()) {
        url += "?";
        url += std::string{ query };
    }
    return url;
}

UpstreamPreparedRequest UpstreamExecutor::prepare(long long channel_id, UpstreamRequest downstream,
                                                  bool retried_unsupported_parameter, bool enforce_ssrf) const
{
    const auto channel = ChannelStore::instance().find_channel(channel_id);
    if (!channel.has_value() || !channel->status) {
        throw std::runtime_error("channel not found");
    }

    UpstreamPreparedRequest prepared;
    prepared.channel_id = channel_id;
    prepared.base_url = validate_upstream_base_url(channel->base_url);
    if (enforce_ssrf) {
        enforce_upstream_ssrf_guard(prepared.base_url);
    }
    prepared.method = downstream.method.empty() ? "POST" : std::move(downstream.method);
    prepared.retried_unsupported_parameter = retried_unsupported_parameter;
    prepared.body = std::move(downstream.body);

    if (channel->api_key.empty()) {
        throw std::runtime_error("channel api key not found");
    }

    if (channel_type_is_anthropic(channel->type)) {
        if (normalize_path(downstream.path) != "/v1/messages") {
            throw std::invalid_argument("anthropic upstream only supports /v1/messages");
        }
        prepared.headers = copy_headers(downstream.headers);
        downstream.headers.clear();
        erase_header(prepared.headers, "Authorization");
        erase_header(prepared.headers, "X-Api-Key");
        erase_header(prepared.headers, "Accept-Encoding");
        set_header(prepared.headers, "Accept-Encoding", "identity");
        if (trim_ascii(header_value(prepared.headers, "anthropic-version")).empty()) {
            set_header(prepared.headers, "anthropic-version", "2023-06-01");
        }
        set_header(prepared.headers, "x-api-key", channel->api_key);
    } else {
        prepared.headers = copy_headers(downstream.headers);
        downstream.headers.clear();
        erase_header(prepared.headers, "Authorization");
        erase_header(prepared.headers, "X-Api-Key");
        erase_header(prepared.headers, "Accept-Encoding");
        set_header(prepared.headers, "Accept-Encoding", "identity");
        set_header(prepared.headers, "Authorization", "Bearer " + channel->api_key);
    }

    prepared.url = build_upstream_url(prepared.base_url, downstream.path, downstream.query);
    return prepared;
}

UpstreamExecutionResult UpstreamExecutor::execute(long long channel_id, UpstreamRequest downstream,
                                                  const UpstreamTransport &transport, bool enforce_ssrf) const
{
    const auto channel = ChannelStore::instance().find_channel(channel_id);
    if (!channel.has_value() || !channel->status) {
        throw std::runtime_error("channel not found");
    }

    UpstreamExecutionResult result;
    result.request = prepare(channel_id, std::move(downstream), false, enforce_ssrf);
    result.response = transport(result.request);
    return result;
}

bool upstream_channel_allows_private_target(std::string_view base_url)
{
    ValidatedBaseUrl parsed;
    try {
        parsed = validate_upstream_base_url(base_url);
    } catch (const std::invalid_argument &) {
        return false;
    }
    std::string lower;
    lower.reserve(parsed.host.size());
    for (char ch : parsed.host) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower == "localhost" || lower == "localhost.localdomain") {
        return true;
    }
    in_addr ipv4{};
    if (::inet_pton(AF_INET, parsed.host.c_str(), &ipv4) == 1) {
        return (ntohl(ipv4.s_addr) & 0xFF000000u) == 0x7F000000u;
    }
    in6_addr ipv6{};
    if (::inet_pton(AF_INET6, parsed.host.c_str(), &ipv6) == 1) {
        static const unsigned char k_loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        return std::memcmp(ipv6.s6_addr, k_loopback, 16) == 0;
    }
    return false;
}

namespace
{

std::string assert_resolved_addresses_allowed(const ValidatedBaseUrl &base_url, bool allow_private_target)
{
    if (allow_private_target) {
        return {};
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    const std::string port = base_url.port.empty() ? (base_url.scheme == "https" ? "443" : "80") : base_url.port;
    if (::getaddrinfo(base_url.host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
        throw std::runtime_error("upstream DNS resolution failed");
    }
    std::string pin;
    for (addrinfo *item = result; item != nullptr; item = item->ai_next) {
        if (is_safe_upstream_sockaddr(item->ai_addr, item->ai_addrlen)) {
            char ip_str[INET6_ADDRSTRLEN];
            if (item->ai_family == AF_INET) {
                ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(item->ai_addr)->sin_addr, ip_str, sizeof(ip_str));
            } else if (item->ai_family == AF_INET6) {
                ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(item->ai_addr)->sin6_addr, ip_str,
                            sizeof(ip_str));
            } else {
                continue;
            }
            // CURLOPT_RESOLVE format: "host:port:address"
            pin = base_url.host + ":" + port + ":" + std::string{ ip_str };
            break;
        }
    }
    ::freeaddrinfo(result);
    if (pin.empty()) {
        throw std::runtime_error("upstream resolved to blocked address");
    }
    return pin;
}

std::vector<CurlHeader> to_curl_headers(const std::vector<UpstreamHeader> &headers)
{
    std::vector<CurlHeader> out;
    out.reserve(headers.size());
    for (const UpstreamHeader &header : headers) {
        if (header.name == "Host" || header.name == "Content-Length" || header.name == "Connection")
            continue;
        out.push_back({ header.name, header.value });
    }
    return out;
}

std::vector<UpstreamHeader> from_curl_headers(const std::vector<CurlHeader> &headers)
{
    std::vector<UpstreamHeader> out;
    out.reserve(headers.size());
    for (const CurlHeader &header : headers) {
        out.push_back({ header.name, header.value });
    }
    return out;
}

} // namespace

UpstreamTransport make_default_upstream_transport(int timeout_ms, bool allow_private_target)
{
    const int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : k_default_upstream_timeout_ms;
    return [effective_timeout_ms, allow_private_target](const UpstreamPreparedRequest &prepared) {
        return default_upstream_http_transport(prepared, effective_timeout_ms, allow_private_target);
    };
}

UpstreamExecutionResult execute_with_default_transport(const UpstreamExecutor &executor, long long channel_id,
                                                       UpstreamRequest downstream, int timeout_ms,
                                                       bool allow_private_target)
{
    return executor.execute(channel_id, std::move(downstream),
                            make_default_upstream_transport(timeout_ms, allow_private_target), !allow_private_target);
}

UpstreamResponse default_upstream_http_transport(const UpstreamPreparedRequest &prepared, int timeout_ms,
                                                 bool allow_private_target)
{
    const int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : k_default_upstream_timeout_ms;
    if (!allow_private_target) {
        enforce_upstream_ssrf_guard(prepared.base_url);
    }
    std::string dns_pin = assert_resolved_addresses_allowed(prepared.base_url, allow_private_target);

    CurlRequest req;
    req.url = prepared.url;
    req.method = prepared.method;
    req.headers = to_curl_headers(prepared.headers);
    req.initial_body_chunk = prepared.body;
    req.connect_timeout_s = std::max(1L, static_cast<long>(effective_timeout_ms / 1000));
    req.total_timeout_s = std::max(1L, static_cast<long>(effective_timeout_ms / 1000));
    req.dns_pin = std::move(dns_pin);

    auto &curl_pool = pool();
    CurlResponse cres = curl_pool.execute(req);

    UpstreamResponse response;
    response.status_code = cres.status_code;
    response.headers = from_curl_headers(cres.headers);
    response.body = std::move(cres.body);
    return response;
}

UpstreamStreamResponse default_upstream_http_stream_transport(const UpstreamPreparedRequest &prepared, int timeout_ms,
                                                              bool allow_private_target)
{
    const int effective_timeout_ms = timeout_ms > 0 ? timeout_ms : k_default_upstream_timeout_ms;
    if (!allow_private_target) {
        enforce_upstream_ssrf_guard(prepared.base_url);
    }
    std::string dns_pin = assert_resolved_addresses_allowed(prepared.base_url, allow_private_target);

    CurlRequest req;
    req.url = prepared.url;
    req.method = prepared.method;
    req.headers = to_curl_headers(prepared.headers);
    req.initial_body_chunk = prepared.body;
    req.connect_timeout_s = std::max(1L, static_cast<long>(effective_timeout_ms / 1000));
    req.total_timeout_s = std::max(1L, static_cast<long>(effective_timeout_ms / 1000));
    req.dns_pin = std::move(dns_pin);

    CurlMultiPool &curl_pool = pool();
    auto result = curl_pool.execute_stream(req);

    UpstreamStreamResponse response;
    response.request = prepared;
    response.status_code = result.status_code;
    response.headers = from_curl_headers(result.headers);
    response.initial_body = std::move(result.initial_body);
    response.stream.read = std::move(result.stream_read);
    response.stream.close = std::move(result.stream_close);
    response.stream.poll_fd = result.poll_fd;
    return response;
}

} // namespace revlm
