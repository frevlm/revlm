#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "auth/security.hpp"
#include "streaming/body_source.hpp"

namespace revlm
{

struct UpstreamHeader {
    std::string name;
    std::string value;
};

struct UpstreamRequest {
    std::string method = "POST";
    std::string path = "/v1/responses";
    std::string query;
    std::vector<UpstreamHeader> headers;
    std::string body;
    /// Declared request body size, or -1 when unknown (keeps Content-Length
    /// framing when streaming from body_source).
    long long content_length = -1;
    /// Sliding-window body source.  When set, the request body is streamed
    /// from this source instead of `body` (which stays empty), so the proxy
    /// never holds the full body in memory.
    std::shared_ptr<BodySource> body_source;
};

struct UpstreamPreparedRequest {
    long long channel_id = 0;
    ValidatedBaseUrl base_url;
    std::string method = "POST";
    std::string url;
    std::vector<UpstreamHeader> headers;
    std::string body;
    long long content_length = -1;
    std::shared_ptr<BodySource> body_source;
};

struct UpstreamResponse {
    int status_code = 0;
    std::vector<UpstreamHeader> headers;
    std::string body;
};

struct UpstreamExecutionResult {
    UpstreamPreparedRequest request;
    UpstreamResponse response;
};

struct UpstreamReadHandle {
    /// Read the next chunk of the upstream response, blocking up to
    /// idle_timeout_ms for data.  Returns:
    ///   > 0  bytes read into buffer
    ///   0    stream finished cleanly
    ///   < 0  transport error (errno set; ETIMEDOUT = inter-event idle timeout)
    std::function<ssize_t(char *, size_t, int idle_timeout_ms)> read;
    std::function<void()> close;
};

struct UpstreamStreamResponse {
    long long channel_id = 0;
    int status_code = 0;
    std::vector<UpstreamHeader> headers;
    std::string initial_body;
    UpstreamReadHandle stream;
};

using UpstreamTransport = std::function<UpstreamResponse(const UpstreamPreparedRequest &)>;

class UpstreamExecutor {
public:
    UpstreamPreparedRequest prepare(long long channel_id, UpstreamRequest downstream, bool enforce_ssrf = true) const;
    UpstreamExecutionResult execute(long long channel_id, UpstreamRequest downstream,
                                    const UpstreamTransport &transport, bool enforce_ssrf = true) const;
};

std::string build_upstream_url(const ValidatedBaseUrl &base_url, std::string_view downstream_path,
                               std::string_view query);
UpstreamResponse default_upstream_http_transport(const UpstreamPreparedRequest &prepared, int timeout_ms = 30000,
                                                 bool allow_private_target = false);
UpstreamStreamResponse default_upstream_http_stream_transport(const UpstreamPreparedRequest &prepared,
                                                              int timeout_ms = 30000,
                                                              bool allow_private_target = false);

UpstreamTransport make_default_upstream_transport(int timeout_ms, bool allow_private_target = false);
UpstreamExecutionResult execute_with_default_transport(const UpstreamExecutor &executor, long long channel_id,
                                                       UpstreamRequest downstream, int timeout_ms,
                                                       bool allow_private_target = false);
bool upstream_channel_allows_private_target(std::string_view base_url);
bool is_hop_by_hop_header(std::string_view name);

} // namespace revlm
