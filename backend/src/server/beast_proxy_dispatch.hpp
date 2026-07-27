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
#include "users/token_api.hpp"
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

/// Authenticate a Beast HTTP request by extracting the API token from
/// Authorization / x-api-key headers and resolving it via snapshot + DB
/// fallback.  Delegates to the string_view overloads of extract_api_token()
/// and authenticate_api_token() in token_api.hpp.
/// Returns channel_group_id on success; writes user_id and token_id.
inline std::optional<long long>
beast_authenticate_api_token(const boost::beast::http::request<boost::beast::http::string_body> &req,
                             long long &user_id, long long &token_id)
{
    std::string_view auth_header;
    std::string_view api_key_header;
    {
        auto it = req.find(boost::beast::http::field::authorization);
        if (it != req.end())
            auth_header = { it->value().data(), it->value().size() };
    }
    {
        auto it = req.find("x-api-key");
        if (it != req.end())
            api_key_header = { it->value().data(), it->value().size() };
    }
    return authenticate_api_token(auth_header, api_key_header, user_id, token_id);
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
    pr.is_stream = true; // Proxy always sends as stream; Gateway detects actual SSE from response Content-Type.

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
