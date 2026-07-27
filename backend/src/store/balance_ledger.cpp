#include "store/balance_ledger.hpp"

#include <memory>
#include <shared_mutex>

namespace revlm
{
namespace
{

BalanceLedger g_balance_ledger;

} // namespace

BalanceLedger &balance_ledger()
{
    return g_balance_ledger;
}

// ---------------------------------------------------------------------------
// load_balance — insert-only: skip if user already tracked in memory.
// This prevents periodic rebuilds from overwriting in-flight deductions.
// ---------------------------------------------------------------------------
void BalanceLedger::load_balance(long long user_id, int64_t balance_micro)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    // Insert-only: if the user is already in the map, their atomic tracks live
    // deductions — do NOT overwrite with the (stale) DB value.
    if (available_.find(user_id) != available_.end()) {
        return;
    }
    available_[user_id] = std::make_unique<std::atomic<int64_t>>(balance_micro);
}

// ---------------------------------------------------------------------------
// admin_adjust_balance — always overwrites. Only called from the admin
// balance-change API path after the DB has been updated.
// ---------------------------------------------------------------------------
void BalanceLedger::admin_adjust_balance(long long user_id, int64_t balance_micro)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = available_.find(user_id);
    if (it != available_.end()) {
        it->second->store(balance_micro, std::memory_order_release);
    } else {
        available_[user_id] = std::make_unique<std::atomic<int64_t>>(balance_micro);
    }
}

// ---------------------------------------------------------------------------
// has_balance — shared lock for map lookup, then atomic load
// ---------------------------------------------------------------------------
bool BalanceLedger::has_balance(long long user_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = available_.find(user_id);
    if (it == available_.end()) {
        return false;
    }
    return it->second->load(std::memory_order_acquire) > 0;
}

// ---------------------------------------------------------------------------
// deduct — shared lock for map lookup, then atomic fetch_sub
// ---------------------------------------------------------------------------
int64_t BalanceLedger::deduct(long long user_id, int64_t amount_micro)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = available_.find(user_id);
    if (it == available_.end()) {
        // No balance entry — deeply negative, caller should 402.
        return -amount_micro;
    }
    return it->second->fetch_sub(amount_micro, std::memory_order_acq_rel) - amount_micro;
}

// ---------------------------------------------------------------------------
// balance — shared lock for map lookup, then atomic load
// ---------------------------------------------------------------------------
int64_t BalanceLedger::balance(long long user_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = available_.find(user_id);
    if (it == available_.end()) {
        return 0;
    }
    return it->second->load(std::memory_order_acquire);
}

} // namespace revlm
