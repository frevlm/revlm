#include "store/snapshot.hpp"

#include "config/config.hpp"
#include "store/database.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace revlm
{
namespace
{

std::shared_ptr<const Snapshot> g_snapshot;
std::atomic<bool> g_invalidated{ false };
std::mutex g_rebuild_mutex;
std::atomic<bool> g_stop{ false };
std::thread g_thread;
std::once_flag g_thread_started;

// Timestamp of the last completed rebuild, guarded by g_rebuild_mutex.
std::chrono::steady_clock::time_point g_last_rebuild;

void do_rebuild()
{
    auto &db = database();
    auto snap = std::make_shared<Snapshot>();

    // a) active users with balance_usd (for balance ledger seeding in Phase 2)
    auto user_rows = sql_query_rows(db, "SELECT id, balance_usd FROM users WHERE status=1");
    for (const auto &row : user_rows) {
        long long uid = std::stoll(row[0].value_or("0"));
        double bal = std::stod(row[1].value_or("0"));
        snap->balances[uid] = bal;
    }

    // b) active tokens
    auto token_rows = sql_query_rows(db, "SELECT t.id, t.user_id, t.token_hash, t.channel_group_id "
                                         "FROM user_tokens t JOIN users u ON u.id=t.user_id "
                                         "WHERE t.status=1 AND u.status=1");
    for (const auto &row : token_rows) {
        SnapshotToken st;
        st.token_id = std::stoll(row[0].value_or("0"));
        st.user_id = std::stoll(row[1].value_or("0"));
        std::string hash = row[2].value_or("");
        st.group_id = std::stoll(row[3].value_or("0"));
        if (!hash.empty()) {
            snap->tokens[std::move(hash)] = st;
        }
    }

    // c) active channel_groups
    auto group_rows =
        sql_query_rows(db, "SELECT id, name, description, price_multiplier, status FROM channel_groups WHERE status=1");
    std::string group_ids;
    for (size_t i = 0; i < group_rows.size(); ++i) {
        const auto &row = group_rows[i];
        long long gid = std::stoll(row[0].value_or("0"));
        snap->groups.try_emplace(gid, gid, row[1].value_or(""), row[2].value_or(""), std::stod(row[3].value_or("1")),
                                 std::stoi(row[4].value_or("0")) != 0);
        if (i) {
            group_ids += ",";
        }
        group_ids += std::to_string(gid);
    }

    // d) active channels with group membership (channels filled into groups)
    if (!group_ids.empty()) {
        auto chan_rows = sql_query_rows(
            db, "SELECT m.channel_group_id, c.id, c.type, c.name, c.status, c.priority, c.base_url, c.api_key, "
                "c.price_multiplier "
                "FROM channel_group_members m "
                "JOIN channels c ON c.id=m.channel_id "
                "WHERE c.status=1 "
                "ORDER BY m.channel_group_id, m.channel_id");
        for (const auto &row : chan_rows) {
            long long gid = std::stoll(row[0].value_or("0"));
            auto it = snap->groups.find(gid);
            if (it != snap->groups.end()) {
                it->second.channels.push_back(Channel(std::stoll(row[1].value_or("0")), row[2].value_or(""),
                                                      row[3].value_or(""), std::stoi(row[4].value_or("0")) != 0,
                                                      std::stoi(row[5].value_or("0")), row[6].value_or(""),
                                                      row[7].value_or(""), std::stod(row[8].value_or("1"))));
            }
        }
    }

    // Atomic swap — readers on the hot path never block.
    std::atomic_store_explicit(&g_snapshot, std::shared_ptr<const Snapshot>(std::move(snap)),
                               std::memory_order_release);
}

void rebuild_loop()
{
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        if (g_stop.load(std::memory_order_relaxed)) {
            break;
        }

        bool needs_rebuild = g_invalidated.exchange(false, std::memory_order_acquire);
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(g_rebuild_mutex);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_rebuild).count();

        // Always rebuild on the periodic 5 s cycle, but also respond to
        // explicit invalidation if the debounce interval has elapsed.
        if (needs_rebuild || elapsed >= 5000) {
            if (elapsed >= static_cast<long long>(config().routing_rebuild_debounce_ms)) {
                do_rebuild();
                g_last_rebuild = std::chrono::steady_clock::now();
            } else {
                // Debounce has not yet passed — re-raise for the next cycle.
                g_invalidated.store(true, std::memory_order_release);
            }
        }
    }
}

} // namespace

std::shared_ptr<const Snapshot> snapshot_acquire()
{
    return std::atomic_load_explicit(&g_snapshot, std::memory_order_acquire);
}

void snapshot_rebuild()
{
    std::call_once(g_thread_started, []() {
        // First call: do the synchronous rebuild and start the background thread.
        {
            std::lock_guard<std::mutex> lock(g_rebuild_mutex);
            do_rebuild();
            g_last_rebuild = std::chrono::steady_clock::now();
        }
        g_thread = std::thread(rebuild_loop);
    });
}

void snapshot_invalidate()
{
    g_invalidated.store(true, std::memory_order_release);
}

void snapshot_shutdown()
{
    g_stop.store(true);
    if (g_thread.joinable()) {
        g_thread.join();
    }
}

} // namespace revlm
