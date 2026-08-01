#include "streaming/transport.hpp"

#include "config/config.hpp"
#include "streaming/body_feeder.hpp"
#include "streaming/buffer_pool.hpp"
#include "streaming/rate_limit.hpp"
#include "streaming/stream_pump_pool.hpp"
#include "proxy/gateway.hpp"

#include <sys/socket.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace revlm
{

namespace
{

void commit_stream_usage(ProxyRequest &request)
{
    try {
        if (!commit_proxy_usage(request))
            std::cerr << "stream usage commit failed\n";
    } catch (const std::exception &err) {
        std::cerr << "stream usage callback failed: " << err.what() << '\n';
    }
}

/// Concrete request-body ownership for the curl-window backend.
struct BodyWindowState {
    ~BodyWindowState()
    {
        stop();
    }

    void stop()
    {
        if (stopped.exchange(true, std::memory_order_acq_rel))
            return;
        if (feeder)
            feeder->cancel();
        if (client_fd >= 0)
            ::shutdown(client_fd, SHUT_RD);
        if (reader.joinable())
            reader.join();
    }

    std::shared_ptr<BodyFeeder> feeder;
    std::shared_ptr<BodySource> source;
    std::thread reader;
    int client_fd = -1;
    std::atomic_bool stopped{ false };
};

class CurlWindowRuntime {
public:
    CurlWindowRuntime()
        : body_buffers_(64)
    {
    }

    void initialize()
    {
        std::call_once(initialized_, [] {
            const Config &cfg = config();
            upload_rate_limiter().configure(static_cast<long long>(cfg.proxy_upload_rate_limit_kbps) * 1024,
                                            static_cast<long long>(cfg.proxy_upload_rate_limit_classify_mb) * 1024 *
                                                1024);
        });
    }

    BodyWindow open_body_window(int client_fd, std::string_view initial_body, long long body_limit_bytes)
    {
        auto state = std::make_shared<BodyWindowState>();
        state->client_fd = client_fd;
        state->feeder = std::make_shared<BodyFeeder>(&body_buffers_);
        state->feeder->set_body_limit(body_limit_bytes);
        state->source = state->feeder->make_source();
        state->feeder->feed_initial(initial_body);

        state->reader = std::thread([feeder = state->feeder, client_fd] { feeder->run(client_fd); });

        BodyWindow window;
        window.source = state->source;
        window.stop = [state] { state->stop(); };
        window.limit_exceeded = [state] { return state->feeder && state->feeder->limit_exceeded(); };
        return window;
    }

    StreamSubmission submit_stream(GatewayStreamKind kind, int client_fd, std::string_view client_ip,
                                   ProxyRequest request, boost::asio::any_io_executor io_executor)
    {
        initialize();

        auto response_started = std::make_shared<std::atomic_bool>(false);
        auto gate = stream_pump_pool().submit(
            client_ip,
            [kind, client_fd, request = std::move(request), response_started]() mutable {
                auto gateway = make_gateway(kind, request);
                if (!gateway)
                    throw std::runtime_error("stream gateway unavailable");
                gateway->run_stream_writer(client_writer_from_fd(client_fd, response_started), commit_stream_usage);
            },
            std::move(io_executor));

        StreamSubmission submission;
        submission.wait = [gate = std::move(gate)](std::function<void()> callback) mutable {
            gate->async_wait(std::move(callback));
        };
        submission.response_started = std::move(response_started);
        return submission;
    }

    void run_inline_stream(GatewayStreamKind kind, ProxyRequest request, ResponseSink &response)
    {
        initialize();
        auto gateway = make_gateway(kind, request);
        if (!gateway)
            throw std::runtime_error("stream gateway unavailable");
        gateway->run_stream(response, commit_stream_usage);
    }

    void shutdown()
    {
        stream_pump_pool().shutdown();
    }

private:
    std::once_flag initialized_;
    BufferPool body_buffers_;
};

CurlWindowRuntime &runtime()
{
    static CurlWindowRuntime instance;
    return instance;
}

} // namespace

void initialize_streaming()
{
    runtime().initialize();
}

BodyWindow open_stream_body_window(int client_fd, std::string_view initial_body, long long body_limit_bytes)
{
    return runtime().open_body_window(client_fd, initial_body, body_limit_bytes);
}

StreamSubmission submit_stream(GatewayStreamKind kind, int client_fd, std::string_view client_ip, ProxyRequest request,
                               boost::asio::any_io_executor io_executor)
{
    return runtime().submit_stream(kind, client_fd, client_ip, std::move(request), std::move(io_executor));
}

void run_inline_stream(GatewayStreamKind kind, ProxyRequest request, ResponseSink &response)
{
    runtime().run_inline_stream(kind, std::move(request), response);
}

void shutdown_streaming()
{
    runtime().shutdown();
}

} // namespace revlm
