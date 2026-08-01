#pragma once

#include <httplib.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "request/proxy_request.hpp"

namespace revlm
{

void register_http_routes(::httplib::Server &server, const std::shared_ptr<std::atomic_bool> &draining);

ProxyRequest make_request(const ::httplib::Request &req);

} // namespace revlm
