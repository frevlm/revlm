#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "auth/session.hpp"
#include "util/json.hpp"
#include "users/users.hpp"

namespace revlm
{

json register_response(const HttpRequestView &raw_request, std::string_view body, std::string *set_cookie = nullptr);
json login_response(const HttpRequestView &raw_request, std::string_view body, std::string *set_cookie = nullptr);
json self_response(const HttpRequestView &raw_request, std::string *set_cookie = nullptr);
json logout_response(const HttpRequestView &raw_request, std::string *set_cookie = nullptr);
json account_email_response(const HttpRequestView &raw_request, std::string_view body,
                            std::string *set_cookie = nullptr);
json account_password_response(const HttpRequestView &raw_request, std::string_view body,
                               std::string *set_cookie = nullptr);
json web_session_auth_failure_response(const WebSessionAuth &auth, const HttpRequestView &raw_request,
                                       std::string *set_cookie = nullptr);
std::optional<User> api_authenticated_user(const HttpRequestView &raw_request, json &error,
                                           std::string *set_cookie = nullptr);
std::optional<User> api_authenticated_admin(const HttpRequestView &raw_request, json &error,
                                            std::string *set_cookie = nullptr);

} // namespace revlm
