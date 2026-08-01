#pragma once

#include <string>
#include <string_view>

#include "auth/session.hpp"
#include "util/json.hpp"

namespace revlm
{

// Admin-facing usage HTTP handlers.
json admin_dashboard_response(const HttpRequestView &raw_request, std::string *set_cookie = nullptr);
json admin_usage_page_response(const HttpRequestView &raw_request, std::string_view target,
                               std::string *set_cookie = nullptr);
json admin_usage_event_detail_response(const HttpRequestView &raw_request, long long event_id,
                                       std::string *set_cookie = nullptr);
json admin_usage_timeseries_response(const HttpRequestView &raw_request, std::string_view target,
                                     std::string *set_cookie = nullptr);

} // namespace revlm
