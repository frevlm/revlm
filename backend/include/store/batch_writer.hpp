#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace revlm
{

struct DebitRecord {
    long long request_id = 0;
    long long user_id = 0;
    long long token_id = 0;
    long long channel_id = 0;
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_create_1h = 0;
    int cache_create_5m = 0;
    double tier_mult = 1.0;
    std::string service_tier;
    double channel_mult = 1.0;
    int status_code = 0;
    int latency_ms = 0;
    int first_token_latency_ms = 0;
    bool is_stream = false;
    std::string model_name;
    std::string error_class;
    std::string error_message;
    int64_t usd_micro = 0;
    std::string datetime;
};

class BatchWriter {
public:
    BatchWriter();

    // Single-threaded consumer. Call from a dedicated thread.
    // Returns the number of records written in this batch.
    int flush();

    // Force flush regardless of batch size/timer. For tests.
    void flush_now();

    // Enqueue a usage record. Called from io threads (any thread).
    void enqueue(long long request_id, long long user_id, long long token_id, long long channel_id, int input_tokens,
                 int output_tokens, int cache_read_tokens, int cache_create_1h, int cache_create_5m, double tier_mult,
                 std::string service_tier, double channel_mult, int status_code, int latency_ms,
                 int first_token_latency_ms, bool is_stream, std::string model_name, std::string error_class,
                 std::string error_message, int64_t usd_micro, std::string datetime);

    // Drain and flush all remaining records on shutdown.
    void drain();

private:
    friend void batch_writer_start();

    void run();
    int do_flush();
    static std::string format_usd(double value);

    std::mutex mutex_;
    std::queue<DebitRecord> active_;
    std::queue<DebitRecord> ready_;
    std::atomic<bool> stop_{ false };
    std::thread thread_;
    std::chrono::steady_clock::time_point last_flush_;

    static constexpr int kBatchSize = 128;
    static constexpr int kMaxQueueSize = 100000;
    static constexpr int64_t kMicroUsdPerUsd = 1000000;
    static constexpr int kMaxRetries = 5;
};

void batch_writer_start();
void batch_writer_shutdown();

// Global accessor — valid after batch_writer_start(), before batch_writer_shutdown().
BatchWriter &batch_writer();

} // namespace revlm
