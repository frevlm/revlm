#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace revlm
{

/// Thread-safe token bucket rate limiter.
///
/// Tokens refill at a constant rate (bytes/sec) up to a configurable
/// capacity (max burst).  All public methods are mutex-protected.
///
/// Typical usage: create one bucket per stream, call try_consume() before
/// each read/write to determine how many bytes may proceed.
class TokenBucket {
public:
    /// rate_bytes_per_sec: sustained fill rate.
    /// capacity_bytes: maximum token accumulation (burst size).
    TokenBucket(double rate_bytes_per_sec, size_t capacity_bytes);

    /// Try to consume up to `requested` bytes.  Returns the number of bytes
    /// actually allowed through (0..requested).  Caller must wait/retry if
    /// fewer bytes were consumed than needed.  Non-blocking.
    size_t try_consume(size_t requested);

    /// Peek at currently available tokens (refills first, does not deduct).
    /// Useful for two-tier rate limiters that need to check both buckets
    /// before committing to a deduction.
    size_t peek() const;

    /// Current token count (fractional, for monitoring / dashboards).
    double available_tokens() const;

    /// Update the sustained fill rate at runtime (dynamic allocation).
    void set_rate(double rate_bytes_per_sec);

    /// Update the maximum burst capacity at runtime.
    void set_capacity(size_t capacity_bytes);

private:
    /// Force a token refill based on elapsed wall-clock time.  Called
    /// automatically by try_consume(), peek(), and available_tokens().
    void refill() const;

    double rate_; // bytes per second
    size_t capacity_; // max tokens (bytes)
    mutable double tokens_; // current tokens
    mutable std::chrono::steady_clock::time_point last_refill_; // last refill timestamp
    mutable std::mutex mu_;
};

/// Two-tier rate limiter combining a global bucket with per-connection buckets.
///
/// Every try_consume() checks both the global limit and the per-connection
/// limit.  The effective allowance is the minimum of the two.  This ensures
/// that no single connection can starve others while still respecting an
/// aggregate bandwidth cap.
class RateLimiter {
public:
    /// global_rate_bps: total bytes/sec across all connections.
    /// per_conn_rate_bps: max bytes/sec per connection (0 = unlimited).
    /// global_capacity: burst capacity for the global bucket (default 256 KB).
    RateLimiter(double global_rate_bps, double per_conn_rate_bps, size_t global_capacity = 262144);

    /// Try to consume bytes from both global and per-connection buckets.
    /// Returns the number of bytes allowed through.  0 means PAUSE.
    size_t try_consume(size_t connection_id, size_t requested);

    /// Register a new per-connection bucket (at the default per-conn rate).
    void add_connection(size_t connection_id);

    /// Remove a connection's bucket.  Safe to call on unknown ids.
    void remove_connection(size_t connection_id);

    /// Update a specific connection's rate at runtime (dynamic reallocation).
    void set_connection_rate(size_t connection_id, double rate_bps);

    /// Hint for fair-share calculations (stored, not used directly by
    /// try_consume — callers can read it for scheduling decisions).
    void set_connection_count(size_t count);

private:
    TokenBucket global_;
    double per_conn_rate_;
    std::unordered_map<size_t, std::unique_ptr<TokenBucket>> per_conn_;
    std::mutex mu_;
    size_t connection_count_ = 0;
};

} // namespace revlm
