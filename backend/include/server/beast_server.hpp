#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace revlm
{

class BeastServer {
public:
    BeastServer();
    ~BeastServer();

    BeastServer(const BeastServer &) = delete;
    BeastServer &operator=(const BeastServer &) = delete;

    /// Listen on host:port for v1 proxy routes only.
    /// Blocks until `running` becomes false, then drains and returns.
    /// `draining` controls health-check responses (shared with HttpServer).
    int listen(std::string host, int port, std::atomic_bool &running,
               const std::shared_ptr<std::atomic_bool> &draining);

    /// Signal the server to stop gracefully.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace revlm
