#include "usage/usage_api.hpp"

#include "users/user_api.hpp"
#include "users/users.hpp"
#include "models/models.hpp"
#include "request/request.hpp"
#include "util/datetime.hpp"
#include "util/http_query.hpp"
#include "util/json_convert.hpp"
#include "util/json.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <date/date.h>
#include <date/tz.h>
#include <exception>
#include <map>
#include <odb/nullable.hxx>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace revlm
{
namespace
{

struct UsageQueryOptions {
    std::string time_zone = "UTC";
    bool all_time = false;
    std::optional<sys_seconds> start_utc;
    std::optional<sys_seconds> end_exclusive_utc;
    std::optional<long long> token_id;
};

std::string model_icon_url(std::string_view owned_by)
{
    const std::string owner = std::string{ trim_ascii(owned_by) };
    if (owner.empty()) {
        return {};
    }
    return "/assets/model-icons/" + owner +
           (owner == "openai" || owner == "xai" || owner == "openrouter" || owner == "ollama" ? ".svg" : "-color.svg");
}

bool parse_usage_query_options(const std::map<std::string, std::string> &params, UsageQueryOptions &out,
                               std::string &message)
{
    out = UsageQueryOptions{};
    out.time_zone = trim_ascii(query_param_value(params, "tz"));
    if (out.time_zone.empty()) {
        out.time_zone = "UTC";
    }
    if (!zone_exists(out.time_zone)) {
        message = "tz 无效";
        return false;
    }

    const std::string all_time_raw = trim_ascii(query_param_value(params, "all_time"));
    if (!all_time_raw.empty()) {
        bool all_time = false;
        if (!parse_bool_flag(all_time_raw, all_time)) {
            message = "all_time 无效";
            return false;
        }
        out.all_time = all_time;
    }

    const std::string start = trim_ascii(query_param_value(params, "start"));
    const std::string end = trim_ascii(query_param_value(params, "end"));
    if (!start.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(start, y, m, d)) {
            message = "start 无效";
            return false;
        }
        out.start_utc = local_date_to_utc(y, static_cast<unsigned>(m), static_cast<unsigned>(d), out.time_zone);
    }
    if (!end.empty()) {
        int y = 0;
        int m = 0;
        int d = 0;
        if (!parse_date_yyyy_mm_dd(end, y, m, d)) {
            message = "end 无效";
            return false;
        }
        unsigned um = static_cast<unsigned>(m);
        unsigned ud = static_cast<unsigned>(d);
        const sys_seconds end_start = local_date_to_utc(y, um, ud, out.time_zone);
        next_date(y, um, ud);
        out.end_exclusive_utc = local_date_to_utc(y, um, ud, out.time_zone);
        if (*out.end_exclusive_utc <= end_start) {
            out.end_exclusive_utc = end_start + std::chrono::seconds{ 86400 };
        }
    }
    if (out.start_utc.has_value() && out.end_exclusive_utc.has_value() && *out.start_utc >= *out.end_exclusive_utc) {
        message = "日期范围无效";
        return false;
    }

    const std::string token_id_raw = trim_ascii(query_param_value(params, "token_id"));
    if (!token_id_raw.empty()) {
        long long token_id = 0;
        if (!parse_i64(token_id_raw, token_id) || token_id <= 0) {
            message = "token_id 无效";
            return false;
        }
        out.token_id = token_id;
    }
    return true;
}

RequestListFilter filter_from_usage_options(long long user_id, const UsageQueryOptions &options)
{
    RequestListFilter filter;
    filter.user_id = user_id;
    if (options.token_id.has_value()) {
        filter.token_id = options.token_id;
    }
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            filter.start = to_mysql_datetime(*options.start_utc);
        }
        if (options.end_exclusive_utc.has_value()) {
            filter.end_exclusive = to_mysql_datetime(*options.end_exclusive_utc);
        }
    }
    return filter;
}

} // namespace

json request_base_event_json(const Request &req)
{
    json o = to_json(req);
    o["time"] = req.time.empty() ? std::string{} : to_iso8601z(parse_mysql_datetime(req.time));
    o["model"] = req.model_name.null() || req.model_name->empty() ? json(nullptr) : json(*req.model_name);
    o["cost_usd"] = request_detail::decimal_to_string(req.solve_price());
    return o;
}

json request_to_user_event_json(const Request &req)
{
    json o = request_base_event_json(req);
    o["response_id"] = req.response_id.null() ? json(nullptr) : json(*req.response_id);
    o["channel_id"] = req.channel_id > 0 ? json(req.channel_id) : json(nullptr);
    o["cache_creation_tokens"] = req.cache_creation_tokens();
    return o;
}

json aggregate_window(const std::vector<Request> &rows, const UsageQueryOptions &options)
{
    const WindowAccumulator acc = accumulate_window(rows);

    const long long tokens = acc.input_tokens + acc.output_tokens + acc.cache_read_tokens + acc.cache_creation_tokens;
    std::string since;
    std::string until;
    if (!options.all_time) {
        if (options.start_utc.has_value()) {
            since = to_iso8601z(*options.start_utc);
        }
        if (options.end_exclusive_utc.has_value()) {
            until = to_iso8601z(*options.end_exclusive_utc - std::chrono::seconds{ 1 });
        }
    }
    if (since.empty() && acc.min_time.has_value()) {
        since = to_iso8601z(*acc.min_time);
    }
    if (until.empty() && acc.max_time.has_value()) {
        until = to_iso8601z(*acc.max_time);
    }

    double minutes = 1.0;
    if (options.start_utc.has_value() && options.end_exclusive_utc.has_value()) {
        minutes = std::max(1.0, std::chrono::duration<double>(*options.end_exclusive_utc - *options.start_utc).count() /
                                    60.0);
    } else if (acc.min_time.has_value() && acc.max_time.has_value() && *acc.max_time >= *acc.min_time) {
        minutes =
            std::max(1.0, std::chrono::duration<double>(*acc.max_time - *acc.min_time).count() / 60.0 + 1.0 / 60.0);
    }

    json window;
    window["window"] = "custom";
    window["since"] = since;
    window["until"] = until;
    window["requests"] = acc.requests;
    window["tokens"] = tokens;
    window["rpm"] = static_cast<long long>(std::llround(static_cast<double>(acc.requests) / minutes));
    window["tpm"] = static_cast<long long>(std::llround(static_cast<double>(tokens) / minutes));
    window["input_tokens"] = acc.input_tokens;
    window["output_tokens"] = acc.output_tokens;
    window["cache_read_tokens"] = acc.cache_read_tokens;
    window["cache_creation_tokens"] = acc.cache_creation_tokens;
    window["cache_ratio"] = acc.input_tokens > 0 ?
                                static_cast<double>(acc.cache_read_tokens + acc.cache_creation_tokens) /
                                    static_cast<double>(acc.input_tokens) :
                                0.0;
    window["first_token_samples"] = acc.first_token_samples;
    window["avg_first_token_latency"] = acc.first_token_samples > 0 ? static_cast<double>(acc.first_token_sum) /
                                                                          static_cast<double>(acc.first_token_samples) :
                                                                      0.0;
    window["tokens_per_second"] = acc.decode_latency_ms > 0 ? static_cast<double>(acc.decode_tokens) * 1000.0 /
                                                                  static_cast<double>(acc.decode_latency_ms) :
                                                              0.0;
    window["usd"] = request_detail::decimal_to_string(acc.used);
    return window;
}

json dashboard_model_stats(const std::vector<Request> &rows)
{
    std::map<std::string, RequestTotal> by_model;
    for (const Request &req : rows) {
        std::string model = req.model_name.null() || req.model_name->empty() ? "" : *req.model_name;
        RequestTotal &total = by_model[model];
        ++total.requests;
        total.tokens += req.total_tokens();
        total.usd += req.solve_price();
    }
    std::vector<std::pair<std::string, RequestTotal>> ranked;
    ranked.reserve(by_model.size());
    for (auto &[name, total] : by_model) {
        ranked.emplace_back(std::move(name), total);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.requests != b.second.requests) {
            return a.second.requests > b.second.requests;
        }
        return a.first < b.first;
    });
    if (ranked.size() > 12) {
        ranked.resize(12);
    }

    // Pre-build model lookup for O(1) access.
    std::unordered_map<std::string, const Model *> model_map;
    for (const Model &model : all_models) {
        model_map[model.name] = &model;
    }

    static constexpr const char *kColors[] = { "#3b82f6", "#22c55e", "#f59e0b", "#ef4444", "#8b5cf6", "#06b6d4" };
    json out = json::array();
    out.as_array().reserve(ranked.size());
    for (size_t i = 0; i < ranked.size(); ++i) {
        json o;
        o["model"] = ranked[i].first;
        const auto it = model_map.find(ranked[i].first);
        const Model *found = it != model_map.end() ? it->second : nullptr;
        const std::string icon = found != nullptr ? model_icon_url(found->owned_by) : "";
        if (icon.empty()) {
            o["icon_url"] = nullptr;
        } else {
            o["icon_url"] = icon;
        }
        o["color"] = kColors[i % (sizeof(kColors) / sizeof(kColors[0]))];
        o["requests"] = ranked[i].second.requests;
        o["tokens"] = ranked[i].second.tokens;
        o["usd"] = request_detail::decimal_to_string(ranked[i].second.usd);
        out.push_back(std::move(o));
    }
    return out;
}

WindowAccumulator accumulate_window(const std::vector<Request> &rows)
{
    WindowAccumulator acc;
    for (const Request &req : rows) {
        ++acc.requests;
        acc.input_tokens += req.input_tokens;
        acc.output_tokens += req.output_tokens;
        acc.cache_read_tokens += req.cache_read_tokens;
        acc.cache_creation_tokens += req.cache_creation_tokens();
        acc.used += req.solve_price();
        if (req.first_token_latency_ms > 0) {
            acc.first_token_sum += req.first_token_latency_ms;
            ++acc.first_token_samples;
        }
        if (req.latency_ms > req.first_token_latency_ms && req.output_tokens > 0) {
            acc.decode_tokens += req.output_tokens;
            acc.decode_latency_ms += req.latency_ms - req.first_token_latency_ms;
        }
        if (!req.time.empty()) {
            try {
                const sys_seconds tp = parse_mysql_datetime(req.time);
                if (!acc.min_time.has_value() || tp < *acc.min_time) {
                    acc.min_time = tp;
                }
                if (!acc.max_time.has_value() || tp > *acc.max_time) {
                    acc.max_time = tp;
                }
            } catch (const std::exception &) {
            }
        }
    }
    return acc;
}

json usage_time_series(const std::vector<Request> &rows, const std::string &tz, std::string_view granularity)
{
    std::map<std::string, RequestTotal> buckets;
    for (const Request &req : rows) {
        if (req.time.empty()) {
            continue;
        }
        sys_seconds tp;
        try {
            tp = parse_mysql_datetime(req.time);
        } catch (const std::exception &) {
            continue;
        }
        const std::string bucket = granularity == "day" ? day_bucket(tp, tz) : hour_bucket(tp, tz);
        RequestTotal &total = buckets[bucket];
        ++total.requests;
        const long long cache_creation = req.cache_creation_tokens();
        total.input_tokens += req.input_tokens;
        total.output_tokens += req.output_tokens;
        total.cache_read_tokens += req.cache_read_tokens;
        total.cache_creation_tokens += cache_creation;
        total.tokens += req.total_tokens();
        total.usd += req.solve_price();
        total.first_token_latency_sum += std::max(req.first_token_latency_ms, 0);
    }

    json points = json::array();
    points.as_array().reserve(buckets.size());
    for (const auto &[bucket, total] : buckets) {
        const long long cached = total.cache_read_tokens + total.cache_creation_tokens;
        json point;
        point["bucket"] = bucket;
        point["requests"] = total.requests;
        point["tokens"] = total.tokens;
        point["usd"] = total.usd;
        point["cache_ratio"] =
            total.input_tokens > 0 ? static_cast<double>(cached) / static_cast<double>(total.input_tokens) : 0.0;
        point["avg_first_token_latency"] = total.requests > 0 ? static_cast<double>(total.first_token_latency_sum) /
                                                                    static_cast<double>(total.requests) :
                                                                0.0;
        point["tokens_per_second"] = 0.0;
        points.push_back(std::move(point));
    }
    return points;
}

json user_models_detail_response(const HttpRequestView &raw_request, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    json models_json = json::array();
    models_json.as_array().reserve(all_models.size());
    for (const Model &model : all_models) {
        json o;
        o["id"] = model.id;
        o["public_id"] = model.name;
        o["owned_by"] = model.owned_by;
        o["input_usd_per_1m"] = request_detail::price_string(model.input_price);
        o["output_usd_per_1m"] = request_detail::price_string(model.output_price);
        o["cache_read_input_usd_per_1m"] = request_detail::price_string(model.cache_read_price);
        o["cache_creation_input_usd_per_1m"] = request_detail::price_string(model.cache_creation_5m_price);
        o["cache_creation_1h_input_usd_per_1m"] = request_detail::price_string(model.cache_creation_1h_price);
        o["status"] = 1;
        o["icon_url"] = model_icon_url(model.owned_by);
        models_json.push_back(std::move(o));
    }
    return json({ { "success", true }, { "data", std::move(models_json) } });
}

json dashboard_response(const HttpRequestView &raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }

    const auto now = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto local = date::make_zoned(options.time_zone, now).get_local_time();
    const date::year_month_day ymd{ date::floor<date::days>(local) };
    int year = static_cast<int>(ymd.year());
    unsigned month = static_cast<unsigned>(ymd.month());
    unsigned day = static_cast<unsigned>(ymd.day());
    options.all_time = false;
    options.start_utc = local_date_to_utc(year, month, day, options.time_zone);
    next_date(year, month, day);
    options.end_exclusive_utc = local_date_to_utc(year, month, day, options.time_zone);

    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        const json today = aggregate_window(rows, options);
        json charts;
        charts["model_stats"] = dashboard_model_stats(rows);
        charts["time_series_stats"] = usage_time_series(rows, options.time_zone, "hour");
        json body;
        body["today_usage_usd"] = today["usd"];
        body["today_since"] = today["since"];
        body["today_until"] = today["until"];
        body["today_requests"] = today["requests"];
        body["today_tokens"] = today["tokens"];
        body["today_rpm"] = std::to_string(today["rpm"].as_int64().value_or(0));
        body["today_tpm"] = std::to_string(today["tpm"].as_int64().value_or(0));
        body["charts"] = std::move(charts);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json usage_windows_response(const HttpRequestView &raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        json body;
        body["time_zone"] = options.time_zone;
        body["now"] = to_iso8601z(date::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        json windows;
        windows.push_back(aggregate_window(rows, options));
        body["windows"] = std::move(windows);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json requests_response(const HttpRequestView &raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }

    int limit = 50;
    const std::string limit_raw = trim_ascii(query_param_value(params, "limit"));
    if (!limit_raw.empty()) {
        int parsed = 0;
        if (parse_i32(limit_raw, parsed) && parsed > 0 && parsed <= 100) {
            limit = parsed;
        }
    }
    RequestListFilter filter = filter_from_usage_options(user->id, options);
    const std::string before_id_raw = trim_ascii(query_param_value(params, "before_id"));
    if (!before_id_raw.empty()) {
        long long before_id = 0;
        if (parse_i64(before_id_raw, before_id) && before_id > 0) {
            filter.before_id = before_id;
        }
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    if (!q_model.empty()) {
        filter.model_like = q_model;
    }
    filter.limit = limit + 1;

    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        auto loaded = store.query(filter);
        const bool has_extra = static_cast<int>(loaded.size()) > limit;
        if (has_extra) {
            loaded.resize(static_cast<size_t>(limit));
        }
        json body;
        json events = json::array();
        events.as_array().reserve(loaded.size());
        for (const Request &req : loaded) {
            events.push_back(request_to_user_event_json(req));
        }
        body["events"] = std::move(events);
        if (has_extra && !loaded.empty()) {
            body["next_before_id"] = loaded.back().id;
        } else {
            body["next_before_id"] = nullptr;
        }
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json usage_timeseries_response(const HttpRequestView &raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    const auto params = parse_query_map(target);
    UsageQueryOptions options;
    std::string message;
    if (!parse_usage_query_options(params, options, message)) {
        return json({ { "success", false }, { "message", message } });
    }
    std::string granularity = trim_ascii(query_param_value(params, "granularity"));
    if (granularity.empty()) {
        granularity = "day";
    }
    if (granularity != "hour" && granularity != "day") {
        return json({ { "success", false }, { "message", "granularity 无效" } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter_from_usage_options(user->id, options));
        json body;
        body["time_zone"] = options.time_zone;
        body["start"] = options.start_utc.has_value() ? json(to_iso8601z(*options.start_utc)) : json(nullptr);
        body["end"] = options.end_exclusive_utc.has_value() ?
                          json(to_iso8601z(*options.end_exclusive_utc - std::chrono::seconds{ 1 })) :
                          json(nullptr);
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, options.time_zone, granularity);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json usage_event_detail_response(const HttpRequestView &raw_request, long long event_id, std::string *set_cookie)
{
    json error;
    const auto user = api_authenticated_user(raw_request, error, set_cookie);
    if (!user.has_value()) {
        return error;
    }
    if (event_id <= 0) {
        return json({ { "success", false }, { "message", "event_id 无效" } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto req = store.get_by_id(event_id);
        if (!req.has_value() || req->user_id != user->id) {
            return json({ { "success", false }, { "message", "事件不存在" } });
        }
        json body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = to_json(compute_pricing_breakdown(*req));
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

} // namespace revlm
