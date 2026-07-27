#include "net/token_bucket.hpp"

#include <algorithm>
#include <cmath>

namespace revlm
{

// ============================================================================
// TokenBucket
// ============================================================================

TokenBucket::TokenBucket(double rate_bytes_per_sec, size_t capacity_bytes)
    : rate_(rate_bytes_per_sec)
    , capacity_(capacity_bytes)
    , tokens_(static_cast<double>(capacity_bytes))
    , last_refill_(std::chrono::steady_clock::now())
{
}

void TokenBucket::refill() const
{
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - last_refill_).count();

    if (elapsed <= 0.0)
        return;

    tokens_ += rate_ * elapsed;
    if (tokens_ > static_cast<double>(capacity_))
        tokens_ = static_cast<double>(capacity_);

    last_refill_ = now;
}

size_t TokenBucket::try_consume(size_t requested)
{
    std::lock_guard<std::mutex> lock(mu_);
    refill();

    const double avail = tokens_;
    if (avail <= 0.0)
        return 0;

    const size_t consumed = std::min(requested, static_cast<size_t>(avail));
    tokens_ -= static_cast<double>(consumed);
    return consumed;
}

size_t TokenBucket::peek() const
{
    std::lock_guard<std::mutex> lock(mu_);
    refill();
    return static_cast<size_t>(tokens_);
}

double TokenBucket::available_tokens() const
{
    std::lock_guard<std::mutex> lock(mu_);
    refill();
    return tokens_;
}

void TokenBucket::set_rate(double rate_bytes_per_sec)
{
    std::lock_guard<std::mutex> lock(mu_);
    rate_ = rate_bytes_per_sec;
}

void TokenBucket::set_capacity(size_t capacity_bytes)
{
    std::lock_guard<std::mutex> lock(mu_);
    capacity_ = capacity_bytes;
    if (tokens_ > static_cast<double>(capacity_))
        tokens_ = static_cast<double>(capacity_);
}

// ============================================================================
// RateLimiter
// ============================================================================

RateLimiter::RateLimiter(double global_rate_bps, double per_conn_rate_bps, size_t global_capacity)
    : global_(global_rate_bps, global_capacity)
    , per_conn_rate_(per_conn_rate_bps)
{
}

size_t RateLimiter::try_consume(size_t connection_id, size_t requested)
{
    std::lock_guard<std::mutex> lock(mu_);

    // Peek both buckets without deducting — avoids the over-deduction bug
    // where the less-constrained bucket wastes tokens.
    const size_t global_avail = global_.peek();
    if (global_avail == 0)
        return 0;

    // Per-connection gate: optional (0 = unlimited).
    auto it = per_conn_.find(connection_id);
    if (it == per_conn_.end() || per_conn_rate_ == 0.0) {
        // No per-conn limit — deduct from global only.
        return global_.try_consume(requested);
    }

    const size_t conn_avail = it->second->peek();
    const size_t allowed = std::min({ requested, global_avail, conn_avail });
    if (allowed == 0)
        return 0;

    // Deduct allowed from both buckets.  Under RateLimiter::mu_ serialization
    // these cannot race — the peeked values are still available.
    global_.try_consume(allowed);
    it->second->try_consume(allowed);
    return allowed;
}

void RateLimiter::add_connection(size_t connection_id)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto bucket = std::make_unique<TokenBucket>(per_conn_rate_, static_cast<size_t>(per_conn_rate_));
    per_conn_[connection_id] = std::move(bucket);
}

void RateLimiter::remove_connection(size_t connection_id)
{
    std::lock_guard<std::mutex> lock(mu_);
    per_conn_.erase(connection_id);
}

void RateLimiter::set_connection_rate(size_t connection_id, double rate_bps)
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = per_conn_.find(connection_id);
    if (it != per_conn_.end())
        it->second->set_rate(rate_bps);
}

void RateLimiter::set_connection_count(size_t count)
{
    std::lock_guard<std::mutex> lock(mu_);
    connection_count_ = count;
}

} // namespace revlm
