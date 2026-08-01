/// BeastServer — Boost.Beast HTTP server for v1 proxy routes only.
///
/// Handles /v1/chat/completions, /v1/messages, /v1/responses,
/// /v1/responses/input_tokens, and /v1/models (GET).
///
/// Admin, API, and management routes continue to run on the existing
/// httplib HttpServer.  Both servers run on separate ports so that
/// httplib's tight coupling to session-cookie logic is undisturbed.
///
/// Architecture:
///   - One io_context with N threads (hardware concurrency).
///   - asio::co_spawn acceptor coroutine (accept loop).
///   - Per-connection coroutine: async_read_header -> auth -> dispatch.
///   - Request bodies are NEVER buffered: a BodyFeeder thread reads the body
///     off the socket into a bounded sliding window while the pump streams it
///     upstream (O(window) memory regardless of body size).
///   - Streaming responses are offloaded to the selected streaming transport:
///     the whole upstream open + SSE pump runs on a dedicated pump thread
///     (whose curl multi reuses connections), and the connection coroutine
///     waits for transport completion — freeing the io thread for the stream
///     duration.
///   - Connection: close for all responses (simple, matches httplib).

#include "streaming/beast_server.hpp"

#include "streaming/beast_proxy_dispatch.hpp"
#include "streaming/beast_response_sink.hpp"
#include "streaming/transport.hpp"

#include "auth/security.hpp"
#include "config/config.hpp"
#include "proxy/gateway.hpp"
#include "proxy/openai_responses.hpp"
#include "request/proxy_request.hpp"
#include "util/json.hpp"
#include "util/strings.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace revlm
{

// net and beast aliases are provided by beast_response_sink.hpp
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Log an access line (mirrors log_access in http_dispatch.cpp).
static void log_access(std::string_view method, std::string_view target, int status, std::string_view request_id)
{
    std::cerr << "access request_id=" << request_id << " status=" << status << " method=" << method
              << " path=" << redact_request_target(target) << '\n';
}

/// Resolve a client-visible request id (mirrors resolve_request_id in http_dispatch.cpp).
static std::string resolve_request_id(const beast::http::request<beast::http::string_body> &req)
{
    auto get = [&](std::string_view name) -> std::string {
        auto it = req.find(name);
        if (it == req.end())
            return {};
        return trim_ascii(it->value());
    };
    std::string id = get("X-Request-Id");
    if (id.empty())
        id = get("x-client-request-id");
    if (id.empty() || id.size() > 128)
        id = "req_" + boost::uuids::to_string(boost::uuids::random_generator{}());
    return id;
}

/// Extract the path from the request target (strip query string).
static std::string_view target_path(std::string_view target)
{
    auto pos = target.find('?');
    return pos == std::string_view::npos ? target : target.substr(0, pos);
}

/// Extract the remote address string from a TCP endpoint.
static std::string remote_addr_str(const tcp::endpoint &ep)
{
    return ep.address().to_string();
}

/// Commit usage after a proxy operation (mirrors finish_proxy_usage).
static void finish_proxy_usage(ResponseSink & /*res*/, ProxyRequest &pr)
{
    if (pr.upstream.channel_id <= 0)
        return;
    if (!commit_proxy_usage(pr))
        std::cerr << "usage commit failed request_id=" << pr.request_id << '\n';
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

/// Write a JSON error response through a BeastResponseSink.
static void write_json_response(BeastResponseSink &sink, int status, json body)
{
    sink.set_status(status);
    sink.set_reason((status >= 200 && status < 300) ? "OK" : "Error");
    sink.set_content(serialize(body), "application/json; charset=utf-8");
}

/// Write a 404 Not Found JSON response.
static void write_not_found(BeastResponseSink &sink)
{
    write_json_response(sink, 404, json{ { "error", json{ { "message", "not found" } } } });
}

// ---------------------------------------------------------------------------
// Per-request dispatch
// ---------------------------------------------------------------------------

/// Result of dispatching one v1 request.
struct V1DispatchResult {
    V1DispatchResult(StreamWait stream_wait, int http_status, std::shared_ptr<std::atomic_bool> started = {})
        : wait(std::move(stream_wait))
        , status(http_status)
        , response_started(std::move(started))
    {
    }

    /// Non-empty when the response was offloaded to the transport; the
    /// connection coroutine must wait for it before closing the socket.
    StreamWait wait;
    /// HTTP status written for non-offloaded paths (0 = not written).
    int status = 0;
    /// Set by the pump writer after the first complete response write.
    std::shared_ptr<std::atomic_bool> response_started;
};

/// Offload the whole streaming request (upstream open + SSE pump) to a
/// transport thread.  The body flows via pr.http.body_source (the feeder),
/// the response is written straight to the client socket through a raw fd
/// writer.  Returns a completion handle the connection coroutine awaits.
static V1DispatchResult dispatch_v1_stream(GatewayStreamKind kind, beast::tcp_stream &stream, ProxyRequest pr,
                                           std::string_view client_ip)
{
    const int fd = stream.socket().native_handle();
    auto io_ex = stream.get_executor();
    auto submission = submit_stream(kind, fd, client_ip, std::move(pr), std::move(io_ex));
    return { std::move(submission.wait), 0, std::move(submission.response_started) };
}

/// Handle a single v1 proxy request.
/// Auth has already been performed; pr.auth is filled.
static V1DispatchResult dispatch_v1_request(beast::tcp_stream &stream,
                                            const beast::http::request<beast::http::string_body> &req, ProxyRequest pr,
                                            std::string_view path, std::string_view client_ip,
                                            const std::shared_ptr<std::atomic_bool> &draining)
{
    using verb = beast::http::verb;
    BeastResponseSink sink(stream);
    const auto method = req.method();

    auto write_quota_or_proceed = [&](int &written) -> bool {
        if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
            write_json_response(sink, 402, *quota_error);
            written = 402;
            return false;
        }
        return true;
    };

    // ---- /v1/models (GET) ----
    if (method == verb::get && path == "/v1/models") {
        try {
            write_json_response(sink, 200, beast_token_models_response(pr.auth.channel_group_id));
            return { nullptr, 200 };
        } catch (const std::exception &) {
            write_json_response(sink, 502, json("查询模型目录失败"));
            return { nullptr, 502 };
        }
    }

    if (method == verb::get && path.starts_with("/v1/models/")) {
        std::string_view model_id = path.substr(13); // strlen("/v1/models/")
        try {
            bool not_found = false;
            json body = beast_token_model_retrieve_response(model_id, pr.auth.channel_group_id, not_found);
            int status = not_found ? 404 : 200;
            write_json_response(sink, status, std::move(body));
            return { nullptr, status };
        } catch (const std::exception &) {
            write_json_response(sink, 502, json("查询模型目录失败"));
            return { nullptr, 502 };
        }
    }

    // ---- POST /v1/chat/completions ----
    if (method == verb::post && path == "/v1/chat/completions") {
        int written = 0;
        if (!write_quota_or_proceed(written))
            return { nullptr, written };
        pr.is_stream = true;
        return dispatch_v1_stream(GatewayStreamKind::openai_chat, stream, std::move(pr), client_ip);
    }

    // ---- POST /v1/messages ----
    if (method == verb::post && path == "/v1/messages") {
        int written = 0;
        if (!write_quota_or_proceed(written))
            return { nullptr, written };
        pr.is_stream = true;
        return dispatch_v1_stream(GatewayStreamKind::anthropics_messages, stream, std::move(pr), client_ip);
    }

    // ---- POST /v1/responses ----
    if (method == verb::post && path == "/v1/responses") {
        int written = 0;
        if (!write_quota_or_proceed(written))
            return { nullptr, written };
        pr.is_stream = true;
        return dispatch_v1_stream(GatewayStreamKind::openai_responses, stream, std::move(pr), client_ip);
    }

    // ---- POST /v1/responses/input_tokens (non-streaming, small response) ----
    if (method == verb::post && path == "/v1/responses/input_tokens") {
        int written = 0;
        if (!write_quota_or_proceed(written))
            return { nullptr, written };
        handle_responses_proxy_request(pr, sink);
        finish_proxy_usage(sink, pr);
        return { nullptr, 200 };
    }

    // ---- /readyz health check ----
    if (method == verb::get && path == "/readyz") {
        sink.set_status(draining->load() ? 503 : 200);
        sink.set_reason(draining->load() ? "Service Unavailable" : "OK");
        sink.set_content(draining->load() ? std::string{ "draining" } : std::string{ "ok" },
                         "text/plain; charset=utf-8");
        return { nullptr, draining->load() ? 503 : 200 };
    }

    write_not_found(sink);
    return { nullptr, 404 };
}

// ---------------------------------------------------------------------------
// Connection coroutine
// ---------------------------------------------------------------------------

/// Handle one accepted connection.
static net::awaitable<void> handle_connection(beast::tcp_stream stream,
                                              const std::shared_ptr<std::atomic_bool> &draining)
{
    const std::string client_ip = remote_addr_str(stream.socket().remote_endpoint());

    try {
        beast::flat_buffer buf;

        // Parse headers only — the body is streamed through a sliding window
        // for POST /v1 routes, never buffered in memory.  body_limit guards
        // the header phase (it is checked in finish_header against the
        // declared Content-Length).  IMPORTANT: Beast's DEFAULT limit is
        // 1 MiB and `body_limit(0)` means "no body allowed", so the
        // unlimited default must be spelled as the maximum value.
        beast::http::request_parser<beast::http::buffer_body> parser;
        if (config().http_max_body_bytes > 0) {
            parser.body_limit(static_cast<std::uint64_t>(config().http_max_body_bytes));
        } else {
            parser.body_limit(std::numeric_limits<std::uint64_t>::max());
        }
        parser.header_limit(static_cast<std::uint32_t>(config().http_max_header_bytes));

        co_await beast::http::async_read_header(stream, buf, parser, net::use_awaitable);
        if (!parser.is_header_done()) {
            std::cerr << "beast: header not fully parsed\n";
            co_return;
        }

        // Build a request skeleton from the parsed headers.
        beast::http::request<beast::http::string_body> req{ parser.get().method(), parser.get().target(),
                                                            parser.get().version() };
        for (auto const &field : parser.get())
            req.set(field.name_string(), field.value());
        req.prepare_payload();
        const std::string request_id = resolve_request_id(req);

        const std::string_view path = target_path(req.target());

        // ---- Route to v1 proxy handler ----
        if (path.starts_with("/v1/")) {
            // Auth from headers before touching the body.
            long long user_id = 0, token_id = 0;
            const auto channel_group_id = beast_authenticate_api_token(req, user_id, token_id);
            if (!channel_group_id.has_value()) {
                BeastResponseSink sink(stream);
                write_json_response(sink, 401, json{ { "error", json{ { "message", "Unauthorized" } } } });
                log_access(req.method_string(), req.target(), 401, request_id);
                co_return;
            }

            ProxyRequest pr = make_beast_proxy_request(req, client_ip);
            pr.request_id = request_id;
            pr.auth.user_id = user_id;
            pr.auth.token_id = token_id;
            pr.auth.channel_group_id = *channel_group_id;
            if (auto content_length = parser.get().find(beast::http::field::content_length);
                content_length != parser.get().end()) {
                const std::string declared_text{ content_length->value() };
                char *end = nullptr;
                const long long declared = std::strtoll(declared_text.c_str(), &end, 10);
                if (end != nullptr && *end == '\0' && declared >= 0)
                    pr.http.content_length = declared;
            }

            // Sliding-window body: the selected transport owns the feeder
            // thread and bounded buffers.  The HTTP layer only keeps the
            // returned handle alive for this request.
            BodyWindow body_window;
            if (req.method() == beast::http::verb::post) {
                // async_read_header over-reads past the header terminator:
                // any body bytes already pulled into the parser's flat_buffer
                // are handed to the feeder first (the socket holds the rest).
                const std::string_view initial_body{ static_cast<const char *>(buf.data().data()), buf.size() };
                const int body_fd = stream.socket().native_handle();
                body_window = open_stream_body_window(body_fd, initial_body, config().http_max_body_bytes);
                pr.http.body_source = body_window.source;
                buf.consume(buf.size());
            }

            V1DispatchResult result = dispatch_v1_request(stream, req, std::move(pr), path, client_ip, draining);

            if (result.wait) {
                // Offloaded: the io thread is freed for the whole stream.
                co_await async_wait_stream(std::move(result.wait), net::use_awaitable);
            }

            // Stop the feeder before the socket closes.  Runs for both paths:
            // the gate path (the pump has finished) and the inline path
            // (e.g. /v1/responses/input_tokens, whose upstream exchange ran
            // on this io thread while the feeder read the body concurrently).
            int access_status = result.status != 0 ? result.status : 200;
            if (body_window.source) {
                body_window.stop();

                // Streaming body-size accounting (design doc §10): when a
                // configured limit is exceeded the feeder stops reading;
                // do not append a second HTTP response after the pump has
                // already written one.  0 = no limit — the upstream's own
                // 413 passes through.
                if (body_window.limit_exceeded && body_window.limit_exceeded() && result.response_started &&
                    !result.response_started->load(std::memory_order_acquire)) {
                    BeastResponseSink sink(stream);
                    write_json_response(sink, 413, json{ { "error", json{ { "message", "payload too large" } } } });
                    access_status = 413;
                }
            }

            log_access(req.method_string(), req.target(), access_status, request_id);
            co_return;
        }

        // ---- /readyz health check ----
        if (path == "/readyz" && req.method() == beast::http::verb::get) {
            ProxyRequest dummy;
            V1DispatchResult result = dispatch_v1_request(stream, req, std::move(dummy), path, client_ip, draining);
            log_access(req.method_string(), req.target(), result.status, request_id);
            co_return;
        }

        // ---- Unknown route ----
        {
            BeastResponseSink sink(stream);
            write_not_found(sink);
        }
        log_access(req.method_string(), req.target(), 404, request_id);
        co_return;

    } catch (const beast::system_error &e) {
        if (e.code() != beast::http::error::end_of_stream && e.code() != net::error::operation_aborted)
            std::cerr << "beast error: " << e.what() << '\n';
    } catch (const std::exception &e) {
        std::cerr << "beast handler error: " << e.what() << '\n';
    }
}

// ---------------------------------------------------------------------------
// Acceptor coroutine
// ---------------------------------------------------------------------------

static net::awaitable<void> do_listen(tcp::acceptor &acceptor, const std::shared_ptr<std::atomic_bool> &draining)
{
    for (;;) {
        try {
            tcp::socket socket = co_await acceptor.async_accept(net::use_awaitable);
            net::co_spawn(acceptor.get_executor(), handle_connection(beast::tcp_stream(std::move(socket)), draining),
                          net::detached);
        } catch (const boost::system::system_error &) {
            // Acceptor closed (shutdown) or other fatal error — exit cleanly.
            co_return;
        }
    }
}

// ---------------------------------------------------------------------------
// BeastServer
// ---------------------------------------------------------------------------

struct BeastServer::Impl {
    net::io_context ioc{ static_cast<int>(std::thread::hardware_concurrency()) };
    std::vector<std::thread> threads;
    std::unique_ptr<tcp::acceptor> acceptor;
    std::shared_ptr<std::atomic_bool> draining;

    void run_threads()
    {
        const unsigned n = std::max(1u, std::thread::hardware_concurrency());
        threads.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            threads.emplace_back([this] { ioc.run(); });
        }
    }

    void join_threads()
    {
        for (auto &t : threads) {
            if (t.joinable())
                t.join();
        }
        threads.clear();
    }
};

BeastServer::BeastServer()
    : impl_(std::make_unique<Impl>())
{
}

BeastServer::~BeastServer()
{
    stop();
}

int BeastServer::listen(std::string host, int port, std::atomic_bool &running,
                        const std::shared_ptr<std::atomic_bool> &draining)
{
    initialize_streaming();
    impl_->draining = draining;

    tcp::resolver resolver(impl_->ioc);
    beast::error_code ec;
    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec) {
        std::cerr << "beast resolve error: " << ec.message() << '\n';
        return 1;
    }

    impl_->acceptor = std::make_unique<tcp::acceptor>(impl_->ioc, *endpoints.begin());
    impl_->acceptor->set_option(tcp::acceptor::reuse_address(true));

    net::co_spawn(impl_->ioc, do_listen(*impl_->acceptor, draining), net::detached);

    impl_->run_threads();

    // Block until told to stop.
    while (running) {
        std::this_thread::sleep_for(100ms);
    }

    stop();
    return 0;
}

void BeastServer::stop()
{
    if (impl_->acceptor) {
        beast::error_code ec;
        impl_->acceptor->close(ec);
    }
    impl_->ioc.stop();
    impl_->join_threads();
    shutdown_streaming();
}

} // namespace revlm
