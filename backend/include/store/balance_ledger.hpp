#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace revlm
{

class BalanceLedger {
public:
    // Load balance from snapshot (called during rebuild).
    // balance_micro is in micro-USD (1 USD = 1_000_000 micro).
    // Insert-only: if user already tracked in memory, the call is a no-op.
    void load_balance(long long user_id, int64_t balance_micro);

    // Admin-triggered balance overwrite — always stores, for balance-change API path.
    // Takes exclusive lock and overwrites the atomic, so only call when the DB
    // has ground truth after a management write.
    void admin_adjust_balance(long long user_id, int64_t balance_micro);

    // Check if user has positive balance. Hot path.
    bool has_balance(long long user_id) const;

    // Deduct balance. Returns new balance after deduction. If negative, caller should 402.
    // Hot path — lock-free atomic fetch_sub after shared-lock map lookup.
    int64_t deduct(long long user_id, int64_t amount_micro);

    // Get current balance (for display/API).
    int64_t balance(long long user_id) const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<long long, std::unique_ptr<std::atomic<int64_t>>> available_;
};

// Global singleton accessor — constructed at static init, seeded by snapshot rebuild.
BalanceLedger &balance_ledger();

} // namespace revlm
