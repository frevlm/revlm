#pragma once

#include <string>
#include <string_view>

#include "util/json.hpp"

namespace revlm
{

// Admin-facing usage HTTP handlers.
json admin_dashboard_response(std::string_view raw_request, std::string *set_cookie = nullptr);
json admin_usage_page_response(std::string_view raw_request, std::string_view target,
                               std::string *set_cookie = nullptr);
json admin_usage_event_detail_response(std::string_view raw_request, long long event_id,
                                       std::string *set_cookie = nullptr);
json admin_usage_timeseries_response(std::string_view raw_request, std::string_view target,
                                     std::string *set_cookie = nullptr);

} // namespace revlm
