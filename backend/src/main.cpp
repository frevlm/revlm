#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

#include "config/config.hpp"
#include "streaming/beast_server.hpp"
#include "server/http_server.hpp"
#include "store/batch_writer.hpp"
#include "store/database.hpp"
#include "store/schema.hpp"
#include "store/snapshot.hpp"

namespace
{

std::atomic_bool running{ true };
std::atomic_bool shutdown_requested{ false };

void stop_server(int)
{
    shutdown_requested.store(true);
}

} // namespace

int main()
{
    std::signal(SIGINT, stop_server);
    std::signal(SIGTERM, stop_server);

    try {
        revlm::init_config(revlm::load_config_from_env());
        revlm::init_database();
        revlm::ensure_schema(revlm::database());
        std::cerr << "database schema ready\n";
        revlm::snapshot_rebuild();
        std::cerr << "snapshot ready\n";
        revlm::batch_writer_start();
        std::cerr << "batch writer started\n";
        revlm::HttpServer server;
        revlm::BeastServer beast_server;
        int exit_code = 0;
        int beast_exit = 0;
        std::atomic_bool server_done{ false };
        std::atomic_bool beast_done{ false };

        std::thread server_thread([&] {
            exit_code = server.run(running);
            server_done.store(true);
        });

        std::thread beast_thread([&] {
            const std::string &raw = revlm::config().beast_addr;
            const auto colon = raw.rfind(':');
            if (colon == std::string::npos) {
                std::cerr << "beast: invalid listen address \"" << raw << "\"\n";
                beast_done.store(true);
                return;
            }
            const std::string host = colon == 0 ? "0.0.0.0" : raw.substr(0, colon);
            const std::string port_text = raw.substr(colon + 1);
            int port = 0;
            try {
                size_t pos = 0;
                port = std::stoi(port_text, &pos);
                if (pos != port_text.size() || port <= 0 || port > 65535) {
                    throw std::invalid_argument("bad port");
                }
            } catch (const std::exception &) {
                std::cerr << "beast: invalid port in \"" << raw << "\"\n";
                beast_done.store(true);
                return;
            }
            beast_exit = beast_server.listen(host, port, running, server.draining_ptr());
            beast_done.store(true);
        });

        while (!shutdown_requested.load() && !server_done.load() && !beast_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (shutdown_requested.load()) {
            server.drain();
            std::cerr << "revlm C++ skeleton draining; readyz returns 503 for "
                      << revlm::config().shutdown_grace_seconds << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(revlm::config().shutdown_grace_seconds));
            running.store(false);
        }
        if (server_done.load() || beast_done.load()) {
            running.store(false);
        }
        beast_thread.join();
        server_thread.join();
        revlm::batch_writer_shutdown();
        revlm::snapshot_shutdown();
        return exit_code != 0 ? exit_code : beast_exit;
    } catch (const std::exception &err) {
        std::cerr << "failed to start revlm C++ skeleton: " << err.what() << '\n';
        return 1;
    }
}
