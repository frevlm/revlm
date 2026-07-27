#pragma once

#include "channels/channel_groups.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace revlm
{

struct SnapshotToken {
    long long user_id = 0;
    long long token_id = 0;
    long long group_id = 0;
};

struct Snapshot {
    std::unordered_map<std::string, SnapshotToken> tokens; // token_hash -> {user_id, token_id, group_id}
    std::unordered_map<long long, ChannelGroup> groups; // group_id -> Group (channels filled)
    std::unordered_map<long long, double> balances; // user_id -> balance_usd
};

// Returns the current atomic snapshot pointer (lock-free read for hot path).
std::shared_ptr<const Snapshot> snapshot_acquire();

// Synchronous full rebuild. Must succeed or throw — caller should fail startup on error.
// On first call also starts the background rebuild thread.
void snapshot_rebuild();

// Marks snapshot dirty so the background thread performs a rebuild after debounce.
// Called by management API write paths.
void snapshot_invalidate();

} // namespace revlm
