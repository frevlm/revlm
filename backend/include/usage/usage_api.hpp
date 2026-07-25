#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "request/request.hpp"
#include "util/datetime.hpp"
#include "util/json.hpp"

namespace revlm
{

// Shared accumulator type — both user-facing aggregate_window() and admin-facing
// admin_window_summary() build their JSON from this single-pass result.
struct WindowAccumulator {
    long long requests = 0;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_creation_tokens = 0;
    long long first_token_sum = 0;
    long long first_token_samples = 0;
    long long decode_tokens = 0;
    long long decode_latency_ms = 0;
    double used = 0.0;
    std::optional<sys_seconds> min_time;
    std::optional<sys_seconds> max_time;
};

WindowAccumulator accumulate_window(const std::vector<Request> &rows);
json request_base_event_json(const Request &req);
json usage_time_series(const std::vector<Request> &rows, const std::string &tz, std::string_view granularity);

// User-facing usage HTTP handlers.
json user_models_detail_response(std::string_view raw_request, std::string *set_cookie = nullptr);
json dashboard_response(std::string_view raw_request, std::string_view target, std::string *set_cookie = nullptr);
json usage_windows_response(std::string_view raw_request, std::string_view target, std::string *set_cookie = nullptr);
json requests_response(std::string_view raw_request, std::string_view target, std::string *set_cookie = nullptr);
json usage_timeseries_response(std::string_view raw_request, std::string_view target,
                               std::string *set_cookie = nullptr);
json usage_event_detail_response(std::string_view raw_request, long long event_id, std::string *set_cookie = nullptr);

} // namespace revlm
