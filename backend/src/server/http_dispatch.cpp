#include "server/http_dispatch.hpp"
#include "server/http_server.hpp"
#include "server/http/httplib_response_sink.hpp"
#include "auth/security.hpp"
#include "users/users.hpp"
#include "users/user_api.hpp"
#include "users/user_admin_api.hpp"
#include "channels/channel_groups.hpp"
#include "channels/channels.hpp"
#include "config/config.hpp"
#include "models/models.hpp"
#include "proxy/openai_chat.hpp"
#include "proxy/anthropics_messages.hpp"
#include "proxy/openai_responses.hpp"
#include "proxy/gateway.hpp"
#include "users/token_api.hpp"
#include "util/datetime.hpp"
#include "util/json.hpp"
#include "util/json_util.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"
#include "usage/usage_api.hpp"
#include "usage/admin_usage_api.hpp"

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <exception>
#include <httplib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace revlm
{
namespace
{

struct ParsedRequest {
    std::string_view method;
    std::string_view path;
    std::string_view target;
    size_t header_bytes = 0;
    size_t content_length = 0;
    bool invalid_framing = false;
};

struct RequestContext {
    ParsedRequest parsed;
    std::string raw_request;
    std::string usage_event_id;
    std::string client_ip;
    std::string set_cookie;
};

std::string serialize_json_http_bytes(int status, std::string_view reason, const json &body)
{
    const std::string payload = serialize(body);
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Content-Length: " << payload.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    std::string bytes = out.str();
    bytes.append(payload);
    return bytes;
}

std::string build_raw_http_request(const ::httplib::Request &req)
{
    std::ostringstream out;
    out << req.method << ' ' << req.target << " HTTP/1.1\r\n";
    for (const auto &header : req.headers) {
        out << header.first << ": " << header.second << "\r\n";
    }
    out << "\r\n" << req.body;
    return out.str();
}

// Correlation id, not a client contract: OpenAI/Anthropic return it in the response, never require it in.
// Honor a client-supplied id (X-Request-Id, then legacy x-client-request-id); otherwise mint one server-side
// so it is always present for logging/persistence. Oversized ids are untrusted -> replaced, never rejected.
std::string resolve_request_id(const ::httplib::Request &req)
{
    std::string id = trim_ascii(req.get_header_value("X-Request-Id"));
    if (id.empty())
        id = trim_ascii(req.get_header_value("x-client-request-id"));
    return (id.empty() || id.size() > 128) ? "req_" + boost::uuids::to_string(boost::uuids::random_generator{}()) : id;
}

json token_models_response(long long channel_group_id)
{
    try {
        json body;
        body["object"] = "list";
        body["data"] = json::array();
        std::vector<std::string> seen;
        const ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
        if (group.status) {
            for (const Channel &channel : group.channels) {
                if (!channel.status) {
                    continue;
                }
                for (const Model &item : channel.models) {
                    std::string id = trim_ascii(item.name);
                    if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end()) {
                        continue;
                    }
                    seen.push_back(id);
                    body["data"].push_back(json({ { "id", id },
                                                  { "object", "model" },
                                                  { "created", 0 },
                                                  { "owned_by", item.owned_by.empty() ? "revlm" : item.owned_by } }));
                }
            }
        }
        return body;
    } catch (const std::exception &) {
        throw std::runtime_error("查询模型目录失败");
    }
}

json token_model_retrieve_response(std::string_view requested_model_id, long long channel_group_id, bool &not_found)
{
    not_found = false;
    const std::string response_id = trim_ascii(requested_model_id);
    if (response_id.empty()) {
        not_found = true;
        return json{ { "error", json{ { "message", "not found" } } } };
    }

    try {
        const ChannelGroup group = ChannelGroupStore::instance().get_channel_group_by_id(channel_group_id);
        if (group.status) {
            for (const Channel &channel : group.channels) {
                if (!channel.status) {
                    continue;
                }
                if (const Model *model = channel.find_model(response_id)) {
                    return json({ { "id", response_id },
                                  { "object", "model" },
                                  { "created", 0 },
                                  { "owned_by", model->owned_by.empty() ? "revlm" : model->owned_by } });
                }
            }
        }
        not_found = true;
        return json{ { "error", json{ { "message", "not found" } } } };
    } catch (const std::exception &) {
        throw std::runtime_error("查询模型目录失败");
    }
}

json billing_balance_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    try {
        UserStore &store = UserStore::instance();
        return json(
            { { "success", true }, { "data", json{ { "balance_usd", store.get_user_balance_usd(user->id) } } } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

ParsedRequest parsed_request_from_httplib(const ::httplib::Request &req)
{
    ParsedRequest parsed;
    parsed.method = req.method;
    parsed.target = req.target;
    parsed.path = req.path;
    parsed.content_length = req.body.size();
    size_t header_bytes = req.method.size() + req.target.size() + 12;
    for (const auto &header : req.headers) {
        header_bytes += header.first.size() + header.second.size() + 4;
    }
    parsed.header_bytes = header_bytes + 4;
    return parsed;
}

bool validate_parsed_request(const ParsedRequest &parsed, ResponseSink &res)
{
    if (parsed.header_bytes > static_cast<size_t>(config().http_max_header_bytes)) {
        write_json(res, 431, json("request header too large"));
        return false;
    }
    if (parsed.content_length > static_cast<size_t>(config().http_max_body_bytes)) {
        write_json(res, 413, json("payload too large"));
        return false;
    }
    return true;
}

RequestContext make_request_context(const ::httplib::Request &req)
{
    const std::string client_ip = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
    return RequestContext{
        .parsed = parsed_request_from_httplib(req),
        .raw_request = inject_request_metadata(build_raw_http_request(req), client_ip),
        .usage_event_id = "req_" + boost::uuids::to_string(boost::uuids::random_generator{}()),
        .client_ip = client_ip,
    };
}

void log_access(::httplib::Response &res, std::string_view method, std::string_view path, int status)
{
    const std::string request_id = res.get_header_value("X-Request-Id");
    std::cerr << "access request_id=" << request_id << " status=" << status << " method=" << method
              << " path=" << redact_request_target(path) << '\n';
}

::httplib::Server::Handler
make_http_handler(std::function<void(const ::httplib::Request &, ::httplib::Response &, RequestContext &)> handler)
{
    return [handler = std::move(handler)](const ::httplib::Request &req, ::httplib::Response &res) {
        RequestContext ctx = make_request_context(req);
        res.set_header("X-Request-Id", resolve_request_id(req));
        HttplibResponseSink sink(res);
        if (!validate_parsed_request(ctx.parsed, sink)) {
            log_access(res, ctx.parsed.method, ctx.parsed.target, res.status);
            return;
        }
        handler(req, res, ctx);
        log_access(res, ctx.parsed.method, ctx.parsed.target, res.status);
    };
}

::httplib::Server::Handler
make_response_handler(std::function<json(const ::httplib::Request &, RequestContext &)> handler)
{
    return make_http_handler(
        [handler = std::move(handler)](const ::httplib::Request &req, ::httplib::Response &res, RequestContext &ctx) {
            HttplibResponseSink sink(res);
            write_json(sink, 200, handler(req, ctx), ctx.set_cookie);
        });
}

std::optional<long long> path_param_i64(const ::httplib::Request &req, std::string_view name)
{
    const auto it = req.path_params.find(std::string{ name });
    if (it == req.path_params.end()) {
        return std::nullopt;
    }
    return parse_positive_i64_or(it->second);
}

std::string path_param_string(const ::httplib::Request &req, std::string_view name)
{
    const auto it = req.path_params.find(std::string{ name });
    return it == req.path_params.end() ? std::string{} : it->second;
}

class InMemoryHttpServer final : public ::httplib::Server {
public:
    bool process(::httplib::Stream &stream, const std::function<void(::httplib::Request &)> &setup_request)
    {
        bool connection_closed = false;
        // Ubuntu 24.04 ships cpp-httplib 0.14.3 (4-arg). 0.25+ define VERSION_NUM and use addr args.
#ifdef CPPHTTPLIB_VERSION_NUM
        return process_request(stream, "127.0.0.1", 0, "127.0.0.1", 0, true, connection_closed, setup_request);
#else
        return process_request(stream, true, connection_closed, setup_request);
#endif
    }
};
} // namespace

ProxyRequest make_request(const ::httplib::Request &req)
{
    static std::atomic<long long> request_counter{ 0 };
    ProxyRequest pr;
    pr.id = ++request_counter;
    pr.request_id = resolve_request_id(req);
    pr.time = to_mysql_datetime(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()));
    pr.http.method = req.method;
    pr.http.path = req.path;
    pr.http.body = req.body;
    pr.http.client_ip = req.remote_addr.empty() ? "127.0.0.1" : req.remote_addr;
    for (const auto &entry : req.headers) {
        const std::string lower = lowercase_ascii(entry.first);
        if (lower == "authorization" || lower == "x-api-key" || lower == "x-client-request-id") {
            continue;
        }
        pr.http.headers.emplace_back(entry.first, entry.second);
    }
    // Ensure X-Request-Id is present in headers for upstream forwarding.
    {
        bool has_request_id = false;
        for (const auto &kv : pr.http.headers) {
            if (lowercase_ascii(kv.first) == "x-request-id") {
                has_request_id = true;
                break;
            }
        }
        if (!has_request_id) {
            pr.http.headers.emplace_back("X-Request-Id", pr.request_id);
        }
    }
    return pr;
}

void proxy_stream_commit_usage(ProxyRequest &pr)
{
    try {
        if (!commit_proxy_usage(pr)) {
            std::cerr << "stream usage commit failed\n";
        }
    } catch (const std::exception &err) {
        std::cerr << "stream usage callback failed: " << err.what() << '\n';
    }
}

void finish_proxy_usage(ResponseSink &res, ProxyRequest &pr)
{
    (void)res;
    if (pr.upstream.channel_id <= 0) {
        return;
    }
    // Upstream body already written — never replace success with synthetic billing errors.
    if (!commit_proxy_usage(pr)) {
        std::cerr << "usage commit failed request_id=" << pr.request_id << '\n';
    }
}

void register_http_routes(::httplib::Server &server, const std::shared_ptr<std::atomic_bool> &draining)
{
    auto api = [](auto fn) {
        return make_response_handler(
            [fn = std::move(fn)](const ::httplib::Request &req, RequestContext &ctx) -> json { return fn(req, ctx); });
    };
    auto v1_http = [](auto fn) {
        return [fn = std::move(fn)](const ::httplib::Request &req, ::httplib::Response &res) {
            ProxyRequest pr = make_request(req);
            res.set_header("X-Request-Id", resolve_request_id(req));
            if (pr.http.body.size() > static_cast<size_t>(config().http_max_body_bytes)) {
                HttplibResponseSink sink(res);
                write_json(sink, 413, json("payload too large"));
                log_access(res, pr.http.method, pr.http.path, res.status);
                return;
            }
            long long user_id = 0;
            long long token_id = 0;
            const auto channel_group_id = authenticate_api_token(req, user_id, token_id);
            if (!channel_group_id.has_value()) {
                HttplibResponseSink sink(res);
                write_json(sink, 401, json{ { "error", json{ { "message", "Unauthorized" } } } });
                log_access(res, pr.http.method, pr.http.path, res.status);
                return;
            }
            pr.auth.user_id = user_id;
            pr.auth.token_id = token_id;
            pr.auth.channel_group_id = *channel_group_id;
            HttplibResponseSink sink(res);
            fn(req, sink, pr);
            log_access(res, pr.http.method, pr.http.path, res.status);
        };
    };

    server.Get("/readyz", make_http_handler([draining](const ::httplib::Request &req, ::httplib::Response &res,
                                                       RequestContext & /* ctx */) {
                   if (draining->load()) {
                       res.status = 503;
                       res.reason = "Service Unavailable";
                       res.set_header("X-Request-Id", resolve_request_id(req));
                       res.set_content("draining", "text/plain; charset=utf-8");
                       return;
                   }
                   res.status = 200;
                   res.reason = "OK";
                   res.set_header("X-Request-Id", resolve_request_id(req));
                   res.set_content("ok", "text/plain; charset=utf-8");
               }));
    server.Get("/api/user/self", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return self_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/user/logout", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return logout_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/user/models/detail", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return user_models_detail_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/dashboard", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return dashboard_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/windows", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return usage_windows_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/events", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return requests_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/timeseries", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return usage_timeseries_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/request/events/:event_id/detail", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto event_id = path_param_i64(req, "event_id");
                   if (!event_id.has_value()) {
                       return json({ { "success", false }, { "message", "event_id 无效" } });
                   }
                   return usage_event_detail_response(ctx.raw_request, *event_id, &ctx.set_cookie);
               }));
    server.Get("/api/token", api([](const ::httplib::Request &, RequestContext &ctx) {
                   json error;
                   const auto user = api_authenticated_user(ctx.raw_request, error, &ctx.set_cookie);
                   if (!user.has_value()) {
                       return error;
                   }
                   return list_user_tokens_response(*user);
               }));
    server.Post("/api/token", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return create_user_token_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Get("/api/token/:token_id/reveal", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              reveal_user_token_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "token_id 不合法" } });
               }));
    server.Post("/api/token/:token_id/rotate", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto token_id = path_param_i64(req, "token_id");
                    return token_id.has_value() ?
                               rotate_user_token_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                               json({ { "success", false }, { "message", "token_id 不合法" } });
                }));
    server.Post("/api/token/:token_id/revoke", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto token_id = path_param_i64(req, "token_id");
                    return token_id.has_value() ?
                               revoke_user_token_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                               json({ { "success", false }, { "message", "token_id 不合法" } });
                }));
    server.Delete("/api/token/:token_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                      const auto token_id = path_param_i64(req, "token_id");
                      return token_id.has_value() ?
                                 delete_user_token_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                                 json({ { "success", false }, { "message", "token_id 不合法" } });
                  }));
    server.Get("/api/token/:token_id/channel", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ? token_channel_response(ctx.raw_request, *token_id, &ctx.set_cookie) :
                                                 json({ { "success", false }, { "message", "token_id 不合法" } });
               }));
    server.Put("/api/token/:token_id/channel", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto token_id = path_param_i64(req, "token_id");
                   return token_id.has_value() ?
                              set_token_channel_response(ctx.raw_request, *token_id, req.body, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "token_id 不合法" } });
               }));
    server.Post("/api/user/register", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return register_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Post("/api/user/login", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return login_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Post("/api/account/email", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return account_email_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Post("/api/account/password", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return account_password_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Get("/v1/models", v1_http([](const ::httplib::Request &, ResponseSink &res, ProxyRequest &pr) {
                   try {
                       write_json(res, 200, token_models_response(pr.auth.channel_group_id));
                   } catch (const std::exception &) {
                       write_json(res, 502, json("查询模型目录失败"));
                   }
               }));
    server.Get("/v1/models/:model_id", v1_http([](const ::httplib::Request &req, ResponseSink &res, ProxyRequest &pr) {
                   try {
                       bool not_found = false;
                       json body = token_model_retrieve_response(path_param_string(req, "model_id"),
                                                                 pr.auth.channel_group_id, not_found);
                       write_json(res, not_found ? 404 : 200, std::move(body));
                   } catch (const std::exception &) {
                       write_json(res, 502, json("查询模型目录失败"));
                   }
               }));
    server.Post("/v1/chat/completions", v1_http([](const ::httplib::Request &req, ResponseSink &res, ProxyRequest &pr) {
                    if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error);
                        return;
                    }
                    pr.is_stream = true;
                    run_chat_completions_stream(res, std::move(pr), proxy_stream_commit_usage);
                    return;
                }));
    server.Post("/v1/messages", v1_http([](const ::httplib::Request &req, ResponseSink &res, ProxyRequest &pr) {
                    if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error);
                        return;
                    }
                    pr.is_stream = true;
                    run_messages_stream(res, std::move(pr), proxy_stream_commit_usage);
                    return;
                }));
    server.Post("/v1/responses", v1_http([](const ::httplib::Request &req, ResponseSink &res, ProxyRequest &pr) {
                    if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error);
                        return;
                    }
                    pr.is_stream = true;
                    ResponsesProxyExecuteOptions options;
                    options.stream_response = &res;
                    options.on_usage = proxy_stream_commit_usage;
                    auto result = handle_responses_proxy_request(pr, res, options);
                    if (!result.handled_stream) {
                        finish_proxy_usage(res, pr);
                    }
                }));
    server.Post("/v1/responses/input_tokens",
                v1_http([](const ::httplib::Request & /* req */, ResponseSink &res, ProxyRequest &pr) {
                    if (const auto quota_error = paygo_balance_gate(pr.auth.user_id); quota_error.has_value()) {
                        write_json(res, 402, *quota_error);
                        return;
                    }
                    handle_responses_proxy_request(pr, res);
                    finish_proxy_usage(res, pr);
                }));

    server.Get("/api/admin/dashboard", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_dashboard_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Get("/api/admin/request", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_usage_page_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/admin/request/timeseries", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_usage_timeseries_response(ctx.raw_request, ctx.parsed.target, &ctx.set_cookie);
               }));
    server.Get("/api/admin/request/events/:event_id/detail",
               api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto event_id = path_param_i64(req, "event_id");
                   return event_id.has_value() ?
                              admin_usage_event_detail_response(ctx.raw_request, *event_id, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "event_id 无效" } });
               }));
    server.Get("/api/admin/users", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return admin_list_users_response(ctx.raw_request, &ctx.set_cookie);
               }));
    server.Post("/api/admin/users", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    return admin_create_user_response(ctx.raw_request, req.body, &ctx.set_cookie);
                }));
    server.Put("/api/admin/users/:user_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                   const auto user_id = path_param_i64(req, "user_id");
                   return user_id.has_value() ?
                              admin_update_user_response(*user_id, ctx.raw_request, req.body, &ctx.set_cookie) :
                              json({ { "success", false }, { "message", "用户不存在" } });
               }));
    server.Delete("/api/admin/users/:user_id", api([](const ::httplib::Request &req, RequestContext &ctx) {
                      const auto user_id = path_param_i64(req, "user_id");
                      return user_id.has_value() ?
                                 admin_delete_user_response(*user_id, ctx.raw_request, &ctx.set_cookie) :
                                 json({ { "success", false }, { "message", "用户不存在" } });
                  }));
    server.Post("/api/admin/users/:user_id/password", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto user_id = path_param_i64(req, "user_id");
                    return user_id.has_value() ? admin_reset_user_password_response(*user_id, ctx.raw_request, req.body,
                                                                                    &ctx.set_cookie) :
                                                 json({ { "success", false }, { "message", "用户不存在" } });
                }));
    server.Post("/api/admin/users/:user_id/balance", api([](const ::httplib::Request &req, RequestContext &ctx) {
                    const auto user_id = path_param_i64(req, "user_id");
                    return user_id.has_value() ?
                               admin_add_user_balance_response(*user_id, ctx.raw_request, req.body, &ctx.set_cookie) :
                               json({ { "success", false }, { "message", "用户不存在" } });
                }));

    server.Get("/api/billing/balance", api([](const ::httplib::Request &, RequestContext &ctx) {
                   return billing_balance_response(ctx.raw_request, &ctx.set_cookie);
               }));

    auto channel_groups = api([](const ::httplib::Request &req, RequestContext &ctx) {
        const ChannelGroupsParsedRequest parsed{ ctx.parsed.method, ctx.parsed.path, ctx.parsed.target };
        return channel_groups_route(ctx.raw_request, req.body, parsed, &ctx.set_cookie);
    });
    server.Get(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Post(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Put(R"(/api/admin/channel-groups.*)", channel_groups);
    server.Delete(R"(/api/admin/channel-groups.*)", channel_groups);

    auto channels = api([](const ::httplib::Request &req, RequestContext &ctx) {
        const ChannelParsedRequest parsed{ ctx.parsed.method, ctx.parsed.path, ctx.parsed.target };
        return channel_route(ctx.raw_request, req.body, parsed, &ctx.set_cookie);
    });
    server.Get(R"(/api/channel.*)", channels);
    server.Post(R"(/api/channel.*)", channels);
    server.Put(R"(/api/channel.*)", channels);
    server.Delete(R"(/api/channel.*)", channels);
}

std::string handle_http_request(std::string_view request, bool draining)
{
    InMemoryHttpServer server;
    auto draining_flag = std::make_shared<std::atomic_bool>(draining);
    server.set_keep_alive_max_count(1);
    server.set_payload_max_length(static_cast<size_t>(config().http_max_body_bytes));
    register_http_routes(server, draining_flag);

    ::httplib::detail::BufferStream stream;
    (void)stream.write(request.data(), request.size());
    const bool ok = server.process(stream, [](::httplib::Request &req) {
        req.remote_addr = "127.0.0.1";
        req.remote_port = 0;
    });

    const std::string &buffer = stream.get_buffer();
    if (!ok || buffer.size() <= request.size()) {
        return serialize_json_http_bytes(400, "Bad Request", json("bad request"));
    }
    return buffer.substr(request.size());
}

std::string inject_request_metadata(std::string_view request, std::string_view client_ip)
{
    std::string enriched{ request };
    const size_t request_line_end = enriched.find("\r\n");
    if (request_line_end == std::string::npos || client_ip.empty()) {
        return enriched;
    }
    enriched.insert(request_line_end + 2, "X-Revlm-Remote-Ip: " + std::string{ client_ip } + "\r\n");
    enriched.insert(request_line_end + 2, "X-Revlm-Client-Ip: " + std::string{ client_ip } + "\r\n");
    return enriched;
}

} // namespace revlm
