#pragma once

#include "server/response_sink.hpp"

#include <functional>
#include <string_view>

#include "proxy/gateway.hpp"
#include "request/proxy_request.hpp"
#include "util/json.hpp"

namespace revlm
{

class OpenaiChatCompletion : public Gateway {
public:
    OpenaiChatCompletion(ProxyRequest &pr)
        : Gateway(pr)
    {
    }
    void finalize(json &json) override;

protected:
    bool channel_ok(const Channel &channel) const override;
    GatewayStreamKind kind() const override;
    std::string_view upstream_path() const override;
};

json run_chat_completions(ProxyRequest &pr);
void run_chat_completions_stream(ResponseSink &res, ProxyRequest pr,
                                 const std::function<void(ProxyRequest &)> &on_usage);

} // namespace revlm
