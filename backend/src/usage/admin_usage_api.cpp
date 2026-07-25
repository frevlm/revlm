#include "usage/admin_usage_api.hpp"

#include "usage/usage_api.hpp"
#include "users/user_api.hpp"
#include "users/users.hpp"
#include "channels/channels.hpp"
#include "request/request.hpp"
#include "store/database.hpp"
#include "util/datetime.hpp"
#include "util/http_query.hpp"
#include "util/json_convert.hpp"
#include "util/json.hpp"
#include "util/strings.hpp"
#include "util/user_input.hpp"

#include <algorithm>
#include <chrono>
#include <date/date.h>
#include <date/tz.h>
#include <exception>
#include <map>
#include <odb/database.hxx>
#include <odb/mysql/query.hxx>
#include <odb/nullable.hxx>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "revlm_entities-odb.hxx"

namespace revlm
{
namespace
{

constexpr std::string_view kAdminTimeZone = "Asia/Shanghai";
const std::string kAdminTimeZoneStr{ kAdminTimeZone };

std::optional<std::string> nullable_odb_string(const odb::nullable<std::string> &value)
{
    if (value.null() || value->empty()) {
        return std::nullopt;
    }
    return *value;
}

struct AdminUsageRange {
    sys_seconds since_utc{};
    sys_seconds until_utc{};
    std::string start;
    std::string end;
    std::string since_local;
    std::string until_local;
    bool all_time = false;
};

std::optional<AdminUsageRange> resolve_admin_usage_range(const std::map<std::string, std::string> &params,
                                                         sys_seconds now_utc, std::string &error)
{
    error.clear();
    AdminUsageRange out;
    const auto today_local = date::make_zoned(kAdminTimeZoneStr, now_utc).get_local_time();
    const date::year_month_day today_ymd{ date::floor<date::days>(today_local) };
    const std::string today = format_local(now_utc, kAdminTimeZoneStr, "%Y-%m-%d");

    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, out.all_time)) {
        error = "all_time 不合法";
        return std::nullopt;
    }

    std::string start = trim_ascii(query_param_value(params, "start"));
    std::string end = trim_ascii(query_param_value(params, "end"));
    if (out.all_time) {
        RequestStore &store = UserStore::instance().tokens().requests();
        RequestListFilter filter;
        filter.limit = 1;
        filter.order_asc = true;
        const auto first_rows = store.query(filter);
        if (!first_rows.empty() && !first_rows.front().time.empty()) {
            try {
                start = format_local(parse_mysql_datetime(first_rows.front().time), kAdminTimeZoneStr, "%Y-%m-%d");
                end = today;
            } catch (const std::exception &) {
                start.clear();
                end.clear();
            }
        } else {
            start.clear();
            end.clear();
        }
    }
    if (start.empty()) {
        start = today;
    }
    if (end.empty()) {
        end = start;
    }

    int start_y = 0;
    int start_m = 0;
    int start_d = 0;
    int end_y = 0;
    int end_m = 0;
    int end_d = 0;
    if (!parse_date_yyyy_mm_dd(start, start_y, start_m, start_d)) {
        error = "start 不合法（格式：YYYY-MM-DD）";
        return std::nullopt;
    }
    if (!parse_date_yyyy_mm_dd(end, end_y, end_m, end_d)) {
        error = "end 不合法（格式：YYYY-MM-DD）";
        return std::nullopt;
    }
    unsigned sm = static_cast<unsigned>(start_m);
    unsigned sd = static_cast<unsigned>(start_d);
    unsigned em = static_cast<unsigned>(end_m);
    unsigned ed = static_cast<unsigned>(end_d);
    out.since_utc = local_date_to_utc(start_y, sm, sd, kAdminTimeZoneStr);
    const sys_seconds end_start = local_date_to_utc(end_y, em, ed, kAdminTimeZoneStr);
    next_date(end_y, em, ed);
    const sys_seconds end_exclusive = local_date_to_utc(end_y, em, ed, kAdminTimeZoneStr);
    if (out.since_utc >= end_exclusive) {
        error = "start 不能晚于 end";
        return std::nullopt;
    }
    out.start = start;
    out.end = end;
    out.since_local = format_local(out.since_utc, kAdminTimeZoneStr, "%Y-%m-%d %H:%M");
    const sys_seconds today_start = local_date_to_utc(static_cast<int>(today_ymd.year()),
                                                      static_cast<unsigned>(today_ymd.month()),
                                                      static_cast<unsigned>(today_ymd.day()), kAdminTimeZoneStr);
    if (end_start >= today_start) {
        out.end = today;
        out.until_utc = now_utc;
        out.until_local = format_local(now_utc, kAdminTimeZoneStr, "%Y-%m-%d %H:%M");
    } else {
        out.until_utc = end_exclusive;
        out.until_local = format_local(end_exclusive - std::chrono::seconds{ 1 }, kAdminTimeZoneStr, "%Y-%m-%d %H:%M");
    }
    return out;
}

RequestListFilter build_admin_filter(const std::map<std::string, std::string> &params, const AdminUsageRange &range,
                                     int limit, std::string &error)
{
    odb::database &db = database();
    error.clear();
    RequestListFilter filters;
    filters.limit = limit;
    filters.start = to_mysql_datetime(range.since_utc);
    filters.end_exclusive = to_mysql_datetime(range.until_utc);

    auto parse_positive_id = [&](const std::string &name, std::optional<long long> &out) -> bool {
        const std::string raw = trim_ascii(query_param_value(params, name));
        if (raw.empty()) {
            return true;
        }
        long long value = 0;
        if (!parse_i64(raw, value) || value <= 0) {
            error = name + " 不合法";
            return false;
        }
        out = value;
        return true;
    };

    if (!parse_positive_id("user_id", filters.user_id)) {
        return filters;
    }
    if (!parse_positive_id("channel_id", filters.channel_id)) {
        return filters;
    }

    const std::string model = trim_ascii(query_param_value(params, "model"));
    if (!model.empty()) {
        filters.model_exact = model;
    }
    const std::string q_model = trim_ascii(query_param_value(params, "q_model"));
    if (!q_model.empty()) {
        filters.model_like = q_model;
    }

    const std::string q_user = trim_ascii(query_param_value(params, "q_user"));
    if (!q_user.empty()) {
        using uq = odb::query<User>;
        ScopedTransaction t(db);
        for (const User &u :
             db.query<User>(uq::email.like("%" + q_user + "%") || uq::username.like("%" + q_user + "%"))) {
            filters.user_ids.push_back(u.id);
        }
        t.commit();
        if (filters.user_ids.empty()) {
            filters.user_ids.push_back(-1);
        }
    }
    const std::string q_channel = trim_ascii(query_param_value(params, "q_channel"));
    if (!q_channel.empty()) {
        using cq = odb::query<Channel>;
        ScopedTransaction t(db);
        for (const Channel &c : db.query<Channel>(cq::name.like("%" + q_channel + "%"))) {
            filters.channel_ids.push_back(c.id);
        }
        t.commit();
        if (filters.channel_ids.empty()) {
            filters.channel_ids.push_back(-1);
        }
    }

    if (!parse_positive_id("before_id", filters.before_id)) {
        return filters;
    }
    if (!parse_positive_id("after_id", filters.after_id)) {
        return filters;
    }
    if (filters.after_id.has_value()) {
        filters.order_asc = true;
    }
    if (filters.before_id.has_value() && filters.after_id.has_value()) {
        error = "before_id 与 after_id 不能同时使用";
    }
    return filters;
}

json request_to_admin_event_json(const Request &req, std::string_view user_email, std::string_view channel_name)
{
    const long long cached_tokens = req.cache_read_tokens + req.cache_creation_tokens();
    json o = request_base_event_json(req);
    o["user_email"] = user_email;
    if (req.output_tokens > 0 && req.latency_ms > 0) {
        o["tokens_per_second"] = request_detail::decimal_to_string(static_cast<double>(req.output_tokens) * 1000.0 /
                                                                   static_cast<double>(req.latency_ms));
    } else {
        o["tokens_per_second"] = "-";
    }
    o["cached_tokens"] = cached_tokens;
    o["upstream_channel_name"] = channel_name;
    o["response_id"] = req.response_id.null() ? json(nullptr) : json(*req.response_id);
    const auto error_class = nullable_odb_string(req.error_class);
    const auto error_message = nullable_odb_string(req.error_message);
    std::string error_str;
    if (error_class.has_value() && error_message.has_value()) {
        error_str.reserve(error_class->size() + error_message->size() + 3);
        error_str.append(*error_class);
        error_str.append(" (");
        error_str.append(*error_message);
        error_str.append(")");
    } else if (error_class.has_value()) {
        error_str = *error_class;
    } else if (error_message.has_value()) {
        error_str = *error_message;
    }
    o["error"] = error_str;
    return o;
}

json top_users_json(const std::vector<Request> &rows)
{
    struct Acc {
        std::string email;
        std::string role;
        long long status = 0;
        double used = 0.0;
    };
    std::map<long long, Acc> by_user;
    UserStore &users = UserStore::instance();
    for (const Request &req : rows) {
        Acc &acc = by_user[req.user_id];
        if (acc.email.empty()) {
            const User u = users.get_user_by_id(req.user_id);
            acc.email = u.email;
            acc.role = u.role;
            acc.status = u.status;
        }
        acc.used += req.solve_price();
    }
    std::vector<std::pair<long long, Acc>> ranked;
    ranked.reserve(by_user.size());
    for (auto &entry : by_user) {
        ranked.emplace_back(entry.first, std::move(entry.second));
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        if (a.second.used != b.second.used) {
            return a.second.used > b.second.used;
        }
        return a.first > b.first;
    });
    if (ranked.size() > 50) {
        ranked.resize(50);
    }
    json out = json::array();
    out.as_array().reserve(ranked.size());
    for (const auto &entry : ranked) {
        json o;
        o["user_id"] = entry.first;
        o["email"] = entry.second.email;
        o["role"] = entry.second.role;
        o["status"] = entry.second.status;
        o["usd"] = request_detail::decimal_to_string(entry.second.used);
        out.push_back(std::move(o));
    }
    return out;
}

json admin_window_summary(const AdminUsageRange &range, const std::vector<Request> &rows,
                          const std::vector<Request> &recent_rows)
{
    const WindowAccumulator acc = accumulate_window(rows);

    long long recent_requests = 0;
    long long recent_tokens = 0;
    for (const Request &req : recent_rows) {
        ++recent_requests;
        recent_tokens += req.input_tokens + req.output_tokens;
    }
    const double total_tokens = static_cast<double>(acc.input_tokens + acc.output_tokens);
    const double cached_tokens = static_cast<double>(acc.cache_read_tokens + acc.cache_creation_tokens);
    json o;
    o["window"] = "统计区间";
    o["since"] = range.since_local;
    o["until"] = range.until_local;
    o["requests"] = acc.requests;
    o["tokens"] = acc.input_tokens + acc.output_tokens;
    o["input_tokens"] = acc.input_tokens;
    o["output_tokens"] = acc.output_tokens;
    o["cached_tokens"] = acc.cache_read_tokens + acc.cache_creation_tokens;
    o["cache_ratio"] =
        request_detail::decimal_to_string((total_tokens > 0 ? cached_tokens / total_tokens : 0.0) * 100.0);
    o["rpm"] = request_detail::decimal_to_string(static_cast<double>(recent_requests));
    o["tpm"] = request_detail::decimal_to_string(static_cast<double>(recent_tokens));
    o["avg_first_token_latency"] = request_detail::decimal_to_string(
        acc.first_token_samples > 0 ?
            static_cast<double>(acc.first_token_sum) / static_cast<double>(acc.first_token_samples) :
            0.0);
    o["tokens_per_second"] = request_detail::decimal_to_string(acc.decode_latency_ms > 0 ?
                                                                   static_cast<double>(acc.decode_tokens) * 1000.0 /
                                                                       static_cast<double>(acc.decode_latency_ms) :
                                                                   0.0);
    o["usd"] = request_detail::decimal_to_string(acc.used);
    return o;
}

} // namespace

json admin_dashboard_response(std::string_view raw_request, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    try {
        UserStore &users = UserStore::instance();
        ChannelStore &channels = ChannelStore::instance();
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto local = date::make_zoned(kAdminTimeZoneStr, now_utc).get_local_time();
        const date::year_month_day ymd{ date::floor<date::days>(local) };
        const sys_seconds today_start = local_date_to_utc(static_cast<int>(ymd.year()),
                                                          static_cast<unsigned>(ymd.month()),
                                                          static_cast<unsigned>(ymd.day()), kAdminTimeZoneStr);

        RequestListFilter filter;
        filter.start = to_mysql_datetime(today_start);
        filter.end_exclusive = to_mysql_datetime(now_utc);
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filter);

        const WindowAccumulator acc = accumulate_window(rows);
        json stats;
        stats["users_count"] = users.count_users();
        const auto channel_list = channels.list_channels();
        stats["channels_count"] = static_cast<long long>(channel_list.size());
        stats["endpoints_count"] = static_cast<long long>(channel_list.size());
        stats["requests_today"] = acc.requests;
        stats["tokens_today"] = acc.input_tokens + acc.output_tokens;
        stats["input_tokens_today"] = acc.input_tokens;
        stats["output_tokens_today"] = acc.output_tokens;
        stats["cost_today"] = request_detail::decimal_to_string(acc.used);
        json data;
        data["admin_time_zone"] = kAdminTimeZone;
        data["stats"] = std::move(stats);
        return json({ { "success", true }, { "data", std::move(data) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "读取统计失败" } });
    }
}

json admin_usage_page_response(std::string_view raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    const auto params = parse_query_map(target);
    int limit = 50;
    const std::string limit_raw = query_param_value(params, "limit");
    if (!limit_raw.empty() && !parse_i32(limit_raw, limit)) {
        return json({ { "success", false }, { "message", "limit 不合法" } });
    }
    if (limit < 10) {
        limit = 10;
    }
    if (limit > 200) {
        limit = 200;
    }
    bool include_summary = true;
    const std::string summary_raw = query_param_value(params, "summary");
    if (!summary_raw.empty() && !parse_bool_flag(summary_raw, include_summary)) {
        return json({ { "success", false }, { "message", "summary 不合法" } });
    }

    try {
        const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        std::string range_error;
        const auto range = resolve_admin_usage_range(params, now_utc, range_error);
        if (!range.has_value()) {
            return json({ { "success", false }, { "message", range_error } });
        }
        std::string filter_error;
        RequestListFilter page_filter = build_admin_filter(params, *range, limit + 1, filter_error);
        if (!filter_error.empty()) {
            return json({ { "success", false }, { "message", filter_error } });
        }
        RequestStore &store = UserStore::instance().tokens().requests();
        auto loaded = store.query(page_filter);
        const bool after = page_filter.after_id.has_value();
        if (after) {
            std::reverse(loaded.begin(), loaded.end());
        }
        const bool has_extra = static_cast<int>(loaded.size()) > limit;
        if (has_extra) {
            loaded.resize(static_cast<size_t>(limit));
        }

        std::map<long long, std::string> emails;
        std::map<long long, std::string> channel_names;
        UserStore &users = UserStore::instance();
        ChannelStore &channels = ChannelStore::instance();
        for (const Channel &c : channels.list_channels()) {
            channel_names[c.id] = c.name;
        }

        json events = json::array();
        events.as_array().reserve(loaded.size());
        for (const Request &req : loaded) {
            auto it = emails.find(req.user_id);
            if (it == emails.end()) {
                it = emails.emplace(req.user_id, users.get_user_by_id(req.user_id).email).first;
            }
            const auto cit = channel_names.find(req.channel_id);
            events.push_back(
                request_to_admin_event_json(req, it->second, cit != channel_names.end() ? cit->second : ""));
        }

        json data;
        data["admin_time_zone"] = kAdminTimeZone;
        data["now"] = format_local(now_utc, kAdminTimeZoneStr, "%Y-%m-%d %H:%M");
        data["start"] = range->start;
        data["end"] = range->end;
        data["limit"] = limit;
        data["events"] = std::move(events);
        if (has_extra && !loaded.empty()) {
            data["next_before_id"] = loaded.back().id;
        } else {
            data["next_before_id"] = nullptr;
        }
        if ((after || page_filter.before_id.has_value()) && !loaded.empty()) {
            data["prev_after_id"] = loaded.front().id;
        } else {
            data["prev_after_id"] = nullptr;
        }
        data["cursor_active"] = page_filter.before_id.has_value() || page_filter.after_id.has_value();

        if (include_summary) {
            // Three DB queries by design: page (above) for paginated events, summary for
            // full-range window aggregation sans pagination cursor, and recent for 60-second
            // RPM/TPM snapshot.
            RequestListFilter summary_filter = page_filter;
            summary_filter.limit = 0;
            summary_filter.before_id.reset();
            summary_filter.after_id.reset();
            summary_filter.order_asc = false;
            const auto summary_rows = store.query(summary_filter);
            RequestListFilter recent_filter;
            recent_filter.start = to_mysql_datetime(now_utc - std::chrono::seconds{ 60 });
            recent_filter.end_exclusive = to_mysql_datetime(now_utc + std::chrono::seconds{ 1 });
            const auto recent_rows = store.query(recent_filter);
            data["window"] = admin_window_summary(*range, summary_rows, recent_rows);
            data["top_users"] = top_users_json(summary_rows);
        }
        return json({ { "success", true }, { "data", std::move(data) } });
    } catch (const std::exception &err) {
        return json({ { "success", false }, { "message", err.what() } });
    }
}

json admin_usage_event_detail_response(std::string_view raw_request, long long event_id, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    if (event_id <= 0) {
        return json({ { "success", false }, { "message", "event_id 不合法" } });
    }
    try {
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto req = store.get_by_id(event_id);
        if (!req.has_value()) {
            return json({ { "success", false }, { "message", "not found" } });
        }
        json body;
        body["event_id"] = req->id;
        body["pricing_breakdown"] = to_json(compute_pricing_breakdown(*req));
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查询失败" } });
    }
}

json admin_usage_timeseries_response(std::string_view raw_request, std::string_view target, std::string *set_cookie)
{
    json error;
    if (!api_authenticated_admin(raw_request, error, set_cookie)) {
        return error;
    }
    std::map<std::string, std::string> params = parse_query_map(target);
    std::string granularity = std::string{ trim_ascii(query_param_value(params, "granularity")) };
    if (granularity.empty()) {
        granularity = "hour";
    }
    if (granularity != "hour" && granularity != "day") {
        return json({ { "success", false }, { "message", "granularity 仅支持 hour/day" } });
    }
    bool all_time = false;
    const std::string all_time_raw = query_param_value(params, "all_time");
    if (!all_time_raw.empty() && !parse_bool_flag(all_time_raw, all_time)) {
        return json({ { "success", false }, { "message", "all_time 不合法" } });
    }
    const sys_seconds now_utc = date::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    if (query_param_value(params, "start").empty() && query_param_value(params, "end").empty() && !all_time) {
        if (granularity == "day") {
            params["start"] =
                format_local(now_utc - std::chrono::seconds{ 29 * 24 * 3600 }, kAdminTimeZoneStr, "%Y-%m-%d");
            params["end"] = format_local(now_utc, kAdminTimeZoneStr, "%Y-%m-%d");
        } else {
            params["start"] = format_local(now_utc, kAdminTimeZoneStr, "%Y-%m-%d");
            params["end"] = params["start"];
        }
    }
    try {
        std::string range_error;
        const auto range = resolve_admin_usage_range(params, now_utc, range_error);
        if (!range.has_value()) {
            return json({ { "success", false }, { "message", range_error } });
        }
        std::string filter_error;
        RequestListFilter filters = build_admin_filter(params, *range, 0, filter_error);
        if (!filter_error.empty()) {
            return json({ { "success", false }, { "message", filter_error } });
        }
        RequestStore &store = UserStore::instance().tokens().requests();
        const auto rows = store.query(filters);
        json body;
        body["admin_time_zone"] = kAdminTimeZone;
        body["start"] = range->start;
        body["end"] = range->end;
        body["granularity"] = granularity;
        body["points"] = usage_time_series(rows, kAdminTimeZoneStr, granularity);
        return json({ { "success", true }, { "data", std::move(body) } });
    } catch (const std::exception &) {
        return json({ { "success", false }, { "message", "查询失败" } });
    }
}

} // namespace revlm
