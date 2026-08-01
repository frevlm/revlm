#pragma once

#include <string>
#include <string_view>

namespace revlm
{

struct Config {
    std::string env = "dev";
    std::string addr = ":8080";
    std::string beast_addr = "0.0.0.0:8080";
    std::string db_dsn;
    std::string redis_addr;
    std::string redis_password;
    std::string redis_key_prefix = "revlm";
    std::string site_base_url;
    int shutdown_grace_seconds = 60;
    int http_read_header_timeout_seconds = 5;
    int http_max_header_bytes = 1 << 20;
    // 0 = no proxy-side body limit (default): oversized bodies pass through
    // and the upstream's 413 reaches the client (design doc §10 — the limit
    // is per-channel/CDN configuration, not a protocol constant).
    long long http_max_body_bytes = 0;

    // Four-segment upstream timeout (design doc: high-concurrency-streaming-rewrite §8).
    // Legacy total-timeout knob: 0 = disabled (the default — a 30 s total timeout
    // used to kill 16 MB uploads that legitimately take ~30.5 s).  When set > 0 it
    // still applies as a hard cap via CURLOPT_TIMEOUT.
    int proxy_upstream_timeout_seconds = 0;
    int proxy_upstream_connect_timeout_seconds = 5;
    int proxy_upstream_upload_stall_seconds = 30;
    int proxy_upstream_upload_stall_low_speed_kbps = 10;
    int proxy_upstream_header_timeout_seconds = 120;
    int proxy_upstream_idle_timeout_seconds = 60;
    int proxy_client_write_timeout_seconds = 60;

    // Upload rate limiting (design doc: two-pool-bandwidth-allocation.md).
    // P = pipe rate in KB/s.  0 = disabled (default).  When enabled:
    //   - streams with Content-Length >= 5 MB (or unknown length) join the
    //     limited pool and share the pipe fairly via CURLOPT_MAX_SEND_SPEED;
    //   - normal (small) streams get priority; their measured backlog claims
    //     part of the pipe (budget = P − backlog/1s − 5% headroom);
    //   - per-flow cap floor is 32 KB/s (3× the 10 KB/s upload-stall
    //     detection, so cap changes never trip LOW_SPEED);
    //   - upstream sockets are bounded to 64 KB SO_SNDBUF (design doc:
    //     without it the backlog signal is blind — written ≠ delivered).
    int proxy_upload_rate_limit_kbps = 0;
    int proxy_upload_rate_limit_classify_mb = 5;

    int db_max_open_conns = 8;
    int db_max_idle_conns = 4;
    int redis_db = 0;
    int gateway_retry_base_delay_ms = 300;
    int gateway_retry_max_delay_ms = 1500;
    int routing_rebuild_debounce_ms = 500;
};

int parse_int_config(const std::string &raw, int fallback, std::string_view key);

Config load_config_from_env();
void validate_config(Config &cfg);

void init_config(Config value);
const Config &config();
void reset_config_for_test(Config value);

} // namespace revlm
