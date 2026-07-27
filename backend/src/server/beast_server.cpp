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
///   - Per-connection coroutine: async_read -> parse -> auth -> dispatch.
///   - Synchronous writes via BeastResponseSink (same connection thread).
///   - Connection: close for all responses (simple, matches httplib).

#include "server/beast_server.hpp"

#include "server/beast_proxy_dispatch.hpp"
#include "server/http/beast_response_sink.hpp"

#include "auth/security.hpp"
#include "config/config.hpp"
#include "proxy/anthropics_messages.hpp"
#include "proxy/gateway.hpp"
#include "proxy/openai_chat.hpp"
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
#include <iostream>
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

/// Commit usage for streaming (mirrors proxy_stream_commit_usage).
static void proxy_stream_commit_usage(ProxyRequest &pr)
{
    try {
        if (!commit_proxy_usage(pr))
            std::cerr << "stream usage commit failed\n";
    } catch (const std::exception &err) {
        std::cerr << "stream usage callback failed: " << err.what() << '\n';
    }
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

/// Handle a single v1 proxy request.
/// Auth has already been performed; pr.auth is filled.
/// Returns HTTP status written (or 0 if the response was streamed and
/// the handler took ownership of the socket).
static int dispatch_v1_request(beast::tcp_stream &stream, const beast::http::request<beast::http::string_body> &req,
                               ProxyRequest &pr, std::string_view path, std::string_view request_id,
                               const std::shared_ptr<std::atomic_bool> &draining)
{
    using verb = beast::http::verb;
    BeastResponseSink sink(stream);
    const auto method = req.method();

    // ---- /v1/models (GET) ----
    if (method == verb::get && path == "/v1/models") {
        try {
            write_json_response(sink, 200, beast_token_models_response(pr.auth.channel_group_id));
            return 200;
        } catch (const std::exception &) {
            write_json_response(sink, 502, json("查询模型目录失败"));
            return 502;
        }
    }

    if (method == verb::get && path.starts_with("/v1/models/")) {
        std::string_view model_id = path.substr(13); // strlen("/v1/models/")
        try {
            bool not_found = false;
            json body = beast_token_model_retrieve_response(model_id, pr.auth.channel_group_id, not_found);
            int status = not_found ? 404 : 200;
            write_json_response(sink, status, std::move(body));
            return status;
        } catch (const std::exception &) {
            write_json_response(sink, 502, json("查询模型目录失败"));
            return 502;
        }
    }

    // ---- POST /v1/chat/completions ----
    if (method == verb::post && path == "/v1/chat/completions") {
        if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
            write_json_response(sink, 402, *quota_error);
            return 402;
        }
        pr.is_stream = true;
        run_chat_completions_stream(sink, std::move(pr), proxy_stream_commit_usage);
        return 0; // streamed — status set by stream handler
    }

    // ---- POST /v1/messages ----
    if (method == verb::post && path == "/v1/messages") {
        if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
            write_json_response(sink, 402, *quota_error);
            return 402;
        }
        pr.is_stream = true;
        run_messages_stream(sink, std::move(pr), proxy_stream_commit_usage);
        return 0; // streamed
    }

    // ---- POST /v1/responses ----
    if (method == verb::post && path == "/v1/responses") {
        if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
            write_json_response(sink, 402, *quota_error);
            return 402;
        }
        pr.is_stream = true;
        ResponsesProxyExecuteOptions options;
        options.stream_response = &sink;
        options.on_usage = proxy_stream_commit_usage;
        auto result = handle_responses_proxy_request(pr, sink, options);
        if (!result.handled_stream)
            finish_proxy_usage(sink, pr);
        return result.handled_stream ? 0 : result.stream_status;
    }

    // ---- POST /v1/responses/input_tokens ----
    if (method == verb::post && path == "/v1/responses/input_tokens") {
        if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
            write_json_response(sink, 402, *quota_error);
            return 402;
        }
        handle_responses_proxy_request(pr, sink);
        finish_proxy_usage(sink, pr);
        return 200;
    }

    // ---- /readyz health check ----
    if (method == verb::get && path == "/readyz") {
        sink.set_status(draining->load() ? 503 : 200);
        sink.set_reason(draining->load() ? "Service Unavailable" : "OK");
        sink.set_content(draining->load() ? std::string{ "draining" } : std::string{ "ok" },
                         "text/plain; charset=utf-8");
        return draining->load() ? 503 : 200;
    }

    write_not_found(sink);
    return 404;
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

        // Parse one request.  Keep-alive is not supported — close after
        // every response (matches the existing httplib server which sets
        // keep_alive_max_count=1).
        // buffer_body reads body in chunks via async_read_some instead of
        // buffering the entire body inside the parser.  Chunks are accumulated
        // into a string for dispatch compatibility.
        // TODO: true sliding window requires interleaving curl sending with
        // Beast body reading (async_read_some -> CurlRequest::read_body).
        beast::http::request_parser<beast::http::buffer_body> parser;
        parser.body_limit(static_cast<std::uint64_t>(config().http_max_body_bytes));
        parser.header_limit(static_cast<std::uint32_t>(config().http_max_header_bytes));

        co_await beast::http::async_read_header(stream, buf, parser, net::use_awaitable);
        if (!parser.is_header_done()) {
            std::cerr << "beast: header not fully parsed\n";
            co_return;
        }

        // Accumulate body chunks via async_read_some loop.
        std::string body_accum;
        body_accum.reserve(config().http_max_body_bytes > 0 ?
                               std::min(static_cast<size_t>(config().http_max_body_bytes), size_t{ 65536 }) :
                               65536);

        auto &body = parser.get().body();
        while (!parser.is_done()) {
            std::array<char, 65536> chunk{};
            body.data = chunk.data();
            body.size = sizeof(chunk);

            beast::error_code ec;
            co_await beast::http::async_read_some(stream, buf, parser, net::redirect_error(net::use_awaitable, ec));

            if (ec == beast::http::error::need_buffer)
                continue;

            if (ec) {
                std::cerr << "beast body read error: " << ec.message() << '\n';
                co_return;
            }

            body_accum.append(chunk.data(), body.size);
        }

        // Build a string_body request from parsed headers + accumulated body.
        beast::http::request<beast::http::string_body> req{ parser.get().method(), parser.get().target(),
                                                            parser.get().version() };
        for (auto const &field : parser.get())
            req.set(field.name_string(), field.value());
        req.body() = std::move(body_accum);
        req.prepare_payload();
        const std::string request_id = resolve_request_id(req);

        const std::string_view path = target_path(req.target());

        // ---- Body-size safety valve ----
        if (req.body().size() > static_cast<size_t>(config().http_max_body_bytes)) {
            BeastResponseSink sink(stream);
            write_json_response(sink, 413, json("payload too large"));
            log_access(req.method_string(), req.target(), 413, request_id);
            co_return;
        }

        // ---- Route to v1 proxy handler ----
        if (path.starts_with("/v1/")) {
            ProxyRequest pr = make_beast_proxy_request(req, client_ip);
            pr.request_id = request_id;

            // Auth
            long long user_id = 0, token_id = 0;
            const auto channel_group_id = beast_authenticate_api_token(req, user_id, token_id);
            if (!channel_group_id.has_value()) {
                BeastResponseSink sink(stream);
                write_json_response(sink, 401, json{ { "error", json{ { "message", "Unauthorized" } } } });
                log_access(req.method_string(), req.target(), 401, request_id);
                co_return;
            }
            pr.auth.user_id = user_id;
            pr.auth.token_id = token_id;
            pr.auth.channel_group_id = *channel_group_id;

            int status = dispatch_v1_request(stream, req, pr, path, request_id, draining);
            log_access(req.method_string(), req.target(), status != 0 ? status : 200, request_id);
            co_return;
        }

        // ---- /readyz health check ----
        if (path == "/readyz" && req.method() == beast::http::verb::get) {
            ProxyRequest dummy; // unused for readyz
            int status = dispatch_v1_request(stream, req, dummy, path, request_id, draining);
            log_access(req.method_string(), req.target(), status, request_id);
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
}

} // namespace revlm
