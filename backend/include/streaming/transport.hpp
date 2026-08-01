#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include "streaming/body_source.hpp"
#include "request/proxy_request.hpp"
#include "streaming/response_sink.hpp"

namespace revlm
{

enum class GatewayStreamKind;

/// Handle for a request-body window owned by a streaming transport backend.
///
/// The HTTP layer may attach `source` to ProxyRequest, stop the window after
/// the request, and inspect the limit flag.  It does not know whether the
/// backend uses a feeder thread, a pipe, or another transport mechanism.
struct BodyWindow {
    std::shared_ptr<BodySource> source;
    std::function<void()> stop;
    std::function<bool()> limit_exceeded;
};

/// Type-erased completion notification for a submitted stream.
using StreamWait = std::function<void(std::function<void()>)>;

struct StreamSubmission {
    StreamWait wait;
    std::shared_ptr<std::atomic_bool> response_started;
};

/// Small HTTP-facing API for the current streaming implementation.
///
void initialize_streaming();

BodyWindow open_stream_body_window(int client_fd, std::string_view initial_body, long long body_limit_bytes);

StreamSubmission submit_stream(GatewayStreamKind kind, int client_fd, std::string_view client_ip, ProxyRequest request,
                               boost::asio::any_io_executor io_executor);

/// Run a stream on the caller's response sink.  This is used by the legacy
/// httplib listener; the HTTP route still knows nothing about the gateway or
/// usage-commit implementation.
void run_inline_stream(GatewayStreamKind kind, ProxyRequest request, ResponseSink &response);

void shutdown_streaming();

/// Asio adapter kept outside the HTTP implementation so the concrete
/// completion primitive remains private to the selected backend.
template <typename CompletionToken> auto async_wait_stream(StreamWait wait, CompletionToken &&token)
{
    return boost::asio::async_initiate<CompletionToken, void()>(
        [wait = std::move(wait)](auto handler) mutable {
            auto boxed = std::make_shared<std::decay_t<decltype(handler)>>(std::move(handler));
            if (!wait) {
                std::move (*boxed)();
                return;
            }
            wait([boxed]() mutable { std::move (*boxed)(); });
        },
        token);
}

} // namespace revlm
