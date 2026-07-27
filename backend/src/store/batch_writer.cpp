#include "store/batch_writer.hpp"

#include "store/database.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace revlm
{
namespace
{

std::unique_ptr<BatchWriter> g_writer;

} // namespace

// ---------------------------------------------------------------------------
// BatchWriter implementation
// ---------------------------------------------------------------------------

BatchWriter::BatchWriter()
    : last_flush_(std::chrono::steady_clock::now())
{
}

std::string BatchWriter::format_usd(double value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    return std::string{ buf };
}

// ---------------------------------------------------------------------------
// run() — background thread entry point
// ---------------------------------------------------------------------------
void BatchWriter::run()
{
    while (!stop_.load(std::memory_order_relaxed)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (static_cast<int>(active_.size()) >= kBatchSize) {
                std::swap(active_, ready_);
            } else if (!active_.empty()) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_flush_ >= std::chrono::milliseconds(200)) {
                    std::swap(active_, ready_);
                }
            }
        }

        if (!ready_.empty()) {
            do_flush();
            last_flush_ = std::chrono::steady_clock::now();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Drain whatever is left on shutdown.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!active_.empty()) {
            ready_.push(std::move(active_.front()));
            active_.pop();
        }
    }
    if (!ready_.empty()) {
        do_flush();
    }
}

// ---------------------------------------------------------------------------
// flush() — unconditional swap + write (public, single-threaded consumer)
// ---------------------------------------------------------------------------
int BatchWriter::flush()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::swap(active_, ready_);
    }

    if (ready_.empty()) {
        return 0;
    }

    int count = do_flush();
    last_flush_ = std::chrono::steady_clock::now();
    return count;
}

void BatchWriter::flush_now()
{
    flush();
}

// ---------------------------------------------------------------------------
// enqueue() — called from any io thread
// ---------------------------------------------------------------------------
void BatchWriter::enqueue(long long request_id, long long user_id, long long token_id, long long channel_id,
                          int input_tokens, int output_tokens, int cache_read_tokens, int cache_create_1h,
                          int cache_create_5m, double tier_mult, std::string service_tier, double channel_mult,
                          int status_code, int latency_ms, int first_token_latency_ms, bool is_stream,
                          std::string model_name, std::string error_class, std::string error_message, int64_t usd_micro,
                          std::string datetime)
{
    DebitRecord rec;
    rec.request_id = request_id;
    rec.user_id = user_id;
    rec.token_id = token_id;
    rec.channel_id = channel_id;
    rec.input_tokens = input_tokens;
    rec.output_tokens = output_tokens;
    rec.cache_read_tokens = cache_read_tokens;
    rec.cache_create_1h = cache_create_1h;
    rec.cache_create_5m = cache_create_5m;
    rec.tier_mult = tier_mult;
    rec.service_tier = std::move(service_tier);
    rec.channel_mult = channel_mult;
    rec.status_code = status_code;
    rec.latency_ms = latency_ms;
    rec.first_token_latency_ms = first_token_latency_ms;
    rec.is_stream = is_stream;
    rec.model_name = std::move(model_name);
    rec.error_class = std::move(error_class);
    rec.error_message = std::move(error_message);
    rec.usd_micro = usd_micro;
    rec.datetime = std::move(datetime);

    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(active_.size()) >= kMaxQueueSize) {
        active_.pop(); // drop oldest
        std::fprintf(stderr,
                     "BatchWriter: queue full (%d), dropped oldest record "
                     "(user=%lld, request=%lld)\n",
                     kMaxQueueSize, static_cast<long long>(rec.user_id), static_cast<long long>(rec.request_id));
    }
    active_.push(std::move(rec));
}

// ---------------------------------------------------------------------------
// do_flush() — write ready_ records to DB in a single transaction
// ---------------------------------------------------------------------------
int BatchWriter::do_flush()
{
    // Move ready_ queue into a local vector for indexed access.
    std::vector<DebitRecord> batch;
    batch.reserve(ready_.size());
    while (!ready_.empty()) {
        batch.push_back(std::move(ready_.front()));
        ready_.pop();
    }

    if (batch.empty()) {
        return 0;
    }

    auto &db = database();

    // ---- Pre-aggregate ---------------------------------------------------

    // user_id -> total deduction in micro-USD
    std::unordered_map<long long, int64_t> user_deltas;

    // (user_id, token_id, date) -> aggregated totals
    struct TotalKey {
        long long user_id;
        long long token_id;
        std::string date;
        bool operator==(const TotalKey &o) const
        {
            return user_id == o.user_id && token_id == o.token_id && date == o.date;
        }
    };
    struct TotalKeyHash {
        std::size_t operator()(const TotalKey &k) const
        {
            std::size_t h = std::hash<long long>{}(k.user_id);
            h ^= std::hash<long long>{}(k.token_id) << 1;
            h ^= std::hash<std::string>{}(k.date) << 2;
            return h;
        }
    };
    struct TotalAgg {
        long long requests = 0;
        long long input_tokens = 0;
        long long output_tokens = 0;
        long long cache_read_tokens = 0;
        long long cache_creation_tokens = 0;
        long long tokens = 0;
        double usd = 0.0;
        long long first_token_latency_sum = 0;
    };
    std::unordered_map<TotalKey, TotalAgg, TotalKeyHash> totals;

    for (const auto &rec : batch) {
        user_deltas[rec.user_id] += rec.usd_micro;

        std::string date;
        if (rec.datetime.size() >= 10) {
            date = rec.datetime.substr(0, 10); // "YYYY-MM-DD"
        }

        TotalKey key{ rec.user_id, rec.token_id, date };
        auto &agg = totals[key];
        agg.requests += 1;
        agg.input_tokens += std::max(rec.input_tokens, 0);
        agg.output_tokens += std::max(rec.output_tokens, 0);
        agg.cache_read_tokens += std::max(rec.cache_read_tokens, 0);
        int cc = std::max(rec.cache_create_1h, 0) + std::max(rec.cache_create_5m, 0);
        agg.cache_creation_tokens += cc;
        agg.tokens +=
            std::max(rec.input_tokens, 0) + std::max(rec.output_tokens, 0) + std::max(rec.cache_read_tokens, 0) + cc;
        agg.usd += static_cast<double>(rec.usd_micro) / static_cast<double>(kMicroUsdPerUsd);
        agg.first_token_latency_sum += std::max(rec.first_token_latency_ms, 0);
    }

    // ---- Build SQL -------------------------------------------------------

    std::string sql;
    sql.reserve(batch.size() * 512 + 1024); // rough estimate

    // a) UPDATE users — one per user, relative deduction
    for (const auto &[uid, delta_micro] : user_deltas) {
        double delta_usd = static_cast<double>(delta_micro) / static_cast<double>(kMicroUsdPerUsd);
        sql += "UPDATE users SET balance_usd = balance_usd - ";
        sql += format_usd(delta_usd);
        sql += " WHERE id = ";
        sql += std::to_string(uid);
        sql += ";\n";
    }

    // b) INSERT INTO requests — multi-row
    if (!batch.empty()) {
        sql += "INSERT INTO requests (time, user_id, token_id, input_tokens, output_tokens, "
               "cache_read_tokens, cache_creation_1h_tokens, cache_creation_5m_tokens, "
               "tier_multiplier, service_tier, channel_multiplier, channel_id, "
               "status_code, latency_ms, first_token_latency_ms, "
               "error_class, error_message, is_stream, model, usd) VALUES ";

        for (size_t i = 0; i < batch.size(); ++i) {
            if (i > 0) {
                sql += ", ";
            }
            const auto &rec = batch[i];

            sql += "(";
            sql += sql_quote(db, rec.datetime); // time
            sql += ", " + std::to_string(rec.user_id); // user_id
            sql += ", " + std::to_string(rec.token_id); // token_id
            sql += ", " + std::to_string(rec.input_tokens); // input_tokens
            sql += ", " + std::to_string(rec.output_tokens); // output_tokens
            sql += ", " + std::to_string(rec.cache_read_tokens); // cache_read_tokens
            sql += ", " + std::to_string(rec.cache_create_1h); // cache_creation_1h_tokens
            sql += ", " + std::to_string(rec.cache_create_5m); // cache_creation_5m_tokens
            sql += ", " + format_usd(rec.tier_mult); // tier_multiplier
            sql += ", " + (rec.service_tier.empty() ? std::string("NULL") : sql_quote(db, rec.service_tier));
            sql += ", " + format_usd(rec.channel_mult); // channel_multiplier
            sql += ", " + std::to_string(rec.channel_id); // channel_id
            sql += ", " + std::to_string(rec.status_code); // status_code
            sql += ", " + std::to_string(rec.latency_ms); // latency_ms
            sql += ", " + std::to_string(rec.first_token_latency_ms); // first_token_latency_ms
            sql += ", " + (rec.error_class.empty() ? std::string("NULL") : sql_quote(db, rec.error_class));
            sql += ", " + (rec.error_message.empty() ? std::string("NULL") : sql_quote(db, rec.error_message));
            sql += ", " + std::to_string(rec.is_stream ? 1 : 0); // is_stream
            sql += ", " + (rec.model_name.empty() ? std::string("NULL") : sql_quote(db, rec.model_name));
            sql += ", " + format_usd(static_cast<double>(rec.usd_micro) / static_cast<double>(kMicroUsdPerUsd));
            sql += ")";
        }
        sql += ";\n";
    }

    // c) INSERT INTO request_totals … ON DUPLICATE KEY UPDATE
    if (!totals.empty()) {
        sql += "INSERT INTO request_totals (user_id, token_id, date, requests, input_tokens, output_tokens, "
               "cache_read_tokens, cache_creation_tokens, tokens, usd, first_token_latency_sum) VALUES ";

        bool first_row = true;
        for (const auto &[key, agg] : totals) {
            if (!first_row) {
                sql += ", ";
            }
            first_row = false;

            sql += "(";
            sql += std::to_string(key.user_id);
            sql += ", " + std::to_string(key.token_id);
            sql += ", " + sql_quote(db, key.date);
            sql += ", " + std::to_string(agg.requests);
            sql += ", " + std::to_string(agg.input_tokens);
            sql += ", " + std::to_string(agg.output_tokens);
            sql += ", " + std::to_string(agg.cache_read_tokens);
            sql += ", " + std::to_string(agg.cache_creation_tokens);
            sql += ", " + std::to_string(agg.tokens);
            sql += ", " + format_usd(agg.usd);
            sql += ", " + std::to_string(agg.first_token_latency_sum);
            sql += ")";
        }
        sql += " ON DUPLICATE KEY UPDATE "
               "requests = requests + VALUES(requests), "
               "input_tokens = input_tokens + VALUES(input_tokens), "
               "output_tokens = output_tokens + VALUES(output_tokens), "
               "cache_read_tokens = cache_read_tokens + VALUES(cache_read_tokens), "
               "cache_creation_tokens = cache_creation_tokens + VALUES(cache_creation_tokens), "
               "tokens = tokens + VALUES(tokens), "
               "usd = usd + VALUES(usd), "
               "first_token_latency_sum = first_token_latency_sum + VALUES(first_token_latency_sum);\n";
    }

    // ---- Execute with retry ----------------------------------------------

    for (int retry = 0; retry <= kMaxRetries; ++retry) {
        try {
            ScopedTransaction txn(db);
            sql_exec(db, sql);
            txn.commit();
            return static_cast<int>(batch.size());
        } catch (const std::exception &e) {
            if (retry >= kMaxRetries) {
                std::fprintf(stderr,
                             "BatchWriter: flush failed after %d retries, "
                             "dropping %zu records: %s\n",
                             kMaxRetries, batch.size(), e.what());
                return 0;
            }
            int delay_ms = 100 * (1 << (retry + 1)); // 200, 400, 800, 1600, 3200
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// drain() — signal stop and join the background thread
// ---------------------------------------------------------------------------
void BatchWriter::drain()
{
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

// ---------------------------------------------------------------------------
// Global lifecycle
// ---------------------------------------------------------------------------

void batch_writer_start()
{
    g_writer = std::make_unique<BatchWriter>();
    g_writer->thread_ = std::thread(&BatchWriter::run, g_writer.get());
}

void batch_writer_shutdown()
{
    if (g_writer) {
        g_writer->drain();
    }
}

BatchWriter &batch_writer()
{
    return *g_writer;
}

} // namespace revlm
