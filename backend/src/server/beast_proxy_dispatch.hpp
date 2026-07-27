#pragma once

/// Beast proxy dispatch: builds ProxyRequest from beast::http::request,
/// extracts and authenticates API tokens, detects stream kind from path,
/// and provides model-listing helpers that mirror the anonymous-namespace
/// functions in http_dispatch.cpp but are callable without httplib.

#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "models/models.hpp"
#include "proxy/gateway.hpp"
#include "request/proxy_request.hpp"
#include "store/snapshot.hpp"
#include "users/tokens.hpp"
#include "users/users.hpp"
#include "util/datetime.hpp"
#include "util/json.hpp"
#include "util/strings.hpp"

#include <boost/beast/http.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace revlm
{

// ---------------------------------------------------------------------------
// Token extraction & auth
// ---------------------------------------------------------------------------

/// Extract the raw API token string from Beast request headers.
/// Checks "Authorization: Bearer <token>" and "x-api-key: <token>".
inline std::string beast_extract_api_token(const boost::beast::http::request<boost::beast::http::string_body> &req)
{
    // Authorization: Bearer <token>
    {
        auto it = req.find(boost::beast::http::field::authorization);
        if (it != req.end()) {
            const std::string authorization = trim_ascii(it->value());
            const size_t sep = authorization.find(' ');
            if (sep != std::string::npos) {
                const std::string scheme = lowercase_ascii(trim_ascii(authorization.substr(0, sep)));
                const std::string token = trim_ascii(authorization.substr(sep + 1));
                if (scheme == "bearer" && !token.empty())
                    return token;
            }
        }
    }

    // x-api-key: <token>
    {
        auto it = req.find("x-api-key");
        if (it != req.end()) {
            const std::string api_key = trim_ascii(it->value());
            if (!api_key.empty())
                return api_key;
        }
    }

    return {};
}

/// Authenticate a raw token string using snapshot + DB fallback.
/// Returns channel_group_id on success; writes user_id and token_id.
inline std::optional<long long> beast_authenticate_raw_token(std::string_view raw_token, long long &user_id,
                                                             long long &token_id)
{
    // Snapshot-first fast path (lock-free read).
    const std::string hash = token_hash(raw_token);
    if (auto snapshot = snapshot_acquire()) {
        auto it = snapshot->tokens.find(hash);
        if (it != snapshot->tokens.end()) {
            const SnapshotToken &st = it->second;
            user_id = st.user_id;
            token_id = st.token_id;
            if (st.group_id <= 0)
                return std::nullopt;
            return st.group_id;
        }
    }

    // DB fallback for newly-created tokens during the rebuild window.
    try {
        return UserStore::instance().tokens().resolve_token_channel_group_by_raw_token(raw_token, user_id, token_id);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// ProxyRequest construction
// ---------------------------------------------------------------------------

/// Build a ProxyRequest from a parsed Beast request.
/// Strips auth-sensitive headers (Authorization, x-api-key) and
/// x-client-request-id.  Mint a fresh request id and timestamp.
inline ProxyRequest make_beast_proxy_request(const boost::beast::http::request<boost::beast::http::string_body> &req,
                                             std::string_view remote_addr)
{
    static std::atomic<long long> request_counter{ 0 };

    ProxyRequest pr;
    pr.id = ++request_counter;
    pr.time = to_mysql_datetime(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()));
    pr.http.method = std::string{ req.method_string() };

    // Path only (no query string).
    std::string_view target = req.target();
    auto qpos = target.find('?');
    pr.http.path = std::string{ qpos == std::string_view::npos ? target : target.substr(0, qpos) };

    pr.http.body = req.body();
    pr.http.client_ip = remote_addr.empty() ? "127.0.0.1" : std::string{ remote_addr };

    // Copy headers, stripping auth / correlation fields.
    for (auto it = req.begin(); it != req.end(); ++it) {
        std::string lower = lowercase_ascii(std::string_view{ it->name_string() });
        if (lower == "authorization" || lower == "x-api-key" || lower == "x-client-request-id")
            continue;
        pr.http.headers.emplace_back(it->name_string(), it->value());
    }

    return pr;
}

// ---------------------------------------------------------------------------
// Stream kind detection
// ---------------------------------------------------------------------------

inline std::optional<GatewayStreamKind> detect_stream_kind(std::string_view path)
{
    if (path == "/v1/chat/completions")
        return GatewayStreamKind::openai_chat;
    if (path == "/v1/messages")
        return GatewayStreamKind::anthropics_messages;
    if (path == "/v1/responses")
        return GatewayStreamKind::openai_responses;
    // /v1/responses/input_tokens and /v1/models are non-streaming.
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Model listing (mirrors anonymous-namespace helpers in http_dispatch.cpp)
// ---------------------------------------------------------------------------

inline json beast_token_models_response(long long channel_group_id)
{
    json body;
    body["object"] = "list";
    body["data"] = json::array();

    std::vector<std::string> seen;
    const ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
    if (group.status) {
        for (const Channel &channel : group.channels) {
            if (!channel.status)
                continue;
            for (const Model &item : channel.models) {
                std::string id = trim_ascii(item.name);
                if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end())
                    continue;
                seen.push_back(id);
                body["data"].push_back(json{ { "id", id },
                                             { "object", "model" },
                                             { "created", 0 },
                                             { "owned_by", item.owned_by.empty() ? "revlm" : item.owned_by } });
            }
        }
    }
    return body;
}

inline json beast_token_model_retrieve_response(std::string_view requested_model_id, long long channel_group_id,
                                                bool &not_found)
{
    not_found = false;
    const std::string response_id = trim_ascii(requested_model_id);
    if (response_id.empty()) {
        not_found = true;
        return json{ { "error", json{ { "message", "not found" } } } };
    }

    const ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
    if (group.status) {
        for (const Channel &channel : group.channels) {
            if (!channel.status)
                continue;
            if (const Model *model = channel.find_model(response_id)) {
                return json{ { "id", response_id },
                             { "object", "model" },
                             { "created", 0 },
                             { "owned_by", model->owned_by.empty() ? "revlm" : model->owned_by } };
            }
        }
    }

    not_found = true;
    return json{ { "error", json{ { "message", "not found" } } } };
}

} // namespace revlm
