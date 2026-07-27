#pragma once

/// CurlMultiPool — per-thread libcurl multi-handle HTTP client.
///
/// Replaces internal httplib::Client usage for upstream proxying.
/// - One CURLM* per instance, intended for single-thread use.
/// - Streaming (execute_stream) spawns a worker thread with its own CURLM*.
/// - Forced HTTP/2 (CURL_HTTP_VERSION_2) to avoid h3 negotiation.
/// - Four-segment timeout: connect, upload-stall, total, inter-event idle.
/// - Sliding-window upload via CURLOPT_READFUNCTION + initial_body_chunk.

#include <curl/curl.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace revlm
{

struct CurlHeader {
    std::string name;
    std::string value;
};

/// Describes one upstream HTTP request.  The pool never copies or retains
/// the request — callbacks (read_body, on_response_chunk, …) must remain
/// valid for the lifetime of execute() / execute_stream().
struct CurlRequest {
    /// Full URL including scheme, host, port, path, and query.
    std::string url;

    /// HTTP method (default "POST").
    std::string method = "POST";

    /// Request headers.  "Host", "Content-Length", and "Connection" are
    /// stripped automatically by the pool.
    std::vector<CurlHeader> headers;

    /// First chunk of the request body.  Sent before read_body() is ever
    /// called.  For non-streaming requests this is typically the entire
    /// body.
    std::string initial_body_chunk;

    // ---- body production (sliding window) ----

    /// Called by the curl READFUNCTION when initial_body_chunk is exhausted
    /// and curl needs more bytes.  Must be non-blocking.
    /// Return a non-empty string_view for the next chunk, or empty to signal
    /// end-of-body.  The returned view only needs to be valid until the
    /// callback returns.
    std::function<std::string_view()> read_body;

    // ---- response consumption ----

    /// Called for each response body chunk.  Return false to abort the
    /// transfer early (curl will see a write error).
    std::function<bool(std::string_view)> on_response_chunk;

    /// Called once when the full set of response headers has been received,
    /// before the first body chunk.
    std::function<void(int status_code, const std::vector<CurlHeader> &headers)> on_headers;

    // ---- timeouts ----

    /// TCP / TLS handshake timeout (seconds).  Default 5 s.
    long connect_timeout_s = 5;

    /// Upload stall detection: if the average upload speed drops below this
    /// many bytes per second for upload_low_speed_time seconds the transfer
    /// is aborted.  Default 10 KB/s.
    long upload_low_speed_bytes = 10240;

    /// Upload stall window (seconds).  Default 30 s.
    long upload_low_speed_time = 30;

    /// Hard total-transfer timeout including connect, upload, and download
    /// (seconds).  Default 300 s (5 min).
    long total_timeout_s = 300;
};

/// Result of a completed (non-streaming) request.
struct CurlResponse {
    int status_code = 0;
    std::vector<CurlHeader> headers;
    std::string body;
};

/// Per-thread libcurl multi-handle pool.
///
/// Usage:
///   CurlMultiPool pool;
///   CurlResponse r = pool.execute(req);           // blocks
///   auto s = pool.execute_stream(req);            // returns immediately
///   while (ssize_t n = s.stream_read(buf, sz)) {} // consume
///   s.stream_close();                             // join worker
class CurlMultiPool {
public:
    CurlMultiPool();
    ~CurlMultiPool();

    CurlMultiPool(const CurlMultiPool &) = delete;
    CurlMultiPool &operator=(const CurlMultiPool &) = delete;

    // ---- non-streaming ----

    /// Execute a request and block until the full response is received.
    /// Throws std::runtime_error on transport / protocol error.
    CurlResponse execute(const CurlRequest &request);

    // ---- streaming ----

    struct StreamResult {
        /// HTTP status code, or < 0 on transport error.
        int status_code = 0;

        /// Response headers.
        std::vector<CurlHeader> headers;

        /// Any body bytes that arrived before headers were complete.
        std::string initial_body;

        /// Read next chunk from the upstream response stream.
        /// Returns >0 for bytes read, 0 for EOF, <0 for error.
        /// Blocks with a short internal timeout.
        std::function<ssize_t(char *, size_t)> stream_read;

        /// Close the stream: signals the worker to stop and joins it.
        /// Safe to call multiple times; no-op after first.
        std::function<void()> stream_close;

        /// Pollable fd for data-ready notification.  -1 when not available.
        int poll_fd = -1;
    };

    /// Launch a streaming request.  Returns immediately; the caller reads
    /// response chunks via StreamResult::stream_read.
    /// Blocks internally until response headers arrive (or timeout).
    /// Throws std::runtime_error on connect / header timeout.
    StreamResult execute_stream(const CurlRequest &request);

    /// Drive the pool's internal multi handle for one tick.  Returns the
    /// number of still-running transfers (for this pool's non-streaming
    /// handle only — streaming requests run on their own handles).
    int perform();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace revlm
