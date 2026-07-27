#include "net/curl_multi_pool.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace revlm
{
namespace
{

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

constexpr char kEmptyAcceptEncoding[] = "";

void strip_crlf(std::string_view &line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);
}

int parse_status_code(std::string_view line)
{
    // skip "HTTP/x.y " or "HTTP/2 "
    const char *p = line.data();
    const char *end = p + line.size();
    while (p < end && *p != ' ')
        ++p;
    while (p < end && *p == ' ')
        ++p;
    int code = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        code = code * 10 + static_cast<int>(*p - '0');
        ++p;
    }
    return code > 0 ? code : 0;
}

std::pair<std::string, std::string> parse_header_line(std::string_view line)
{
    const auto colon = line.find(':');
    if (colon == std::string_view::npos)
        return {};
    std::string name{ line.substr(0, colon) };
    std::string_view raw_value = line.substr(colon + 1);
    const auto start = raw_value.find_first_not_of(" \t");
    std::string value = (start != std::string_view::npos) ? std::string{ raw_value.substr(start) } : std::string{};
    return { std::move(name), std::move(value) };
}

// ---------------------------------------------------------------------------
// request context — owns CURL easy handle + read state + response accumulator
// ---------------------------------------------------------------------------

struct RequestContext {
    CurlRequest request;
    CurlResponse response;

    // read side
    std::string body_buffer; // remaining un-sent bytes of current chunk
    size_t body_offset = 0; // read cursor within body_buffer
    bool initial_chunk_sent = false;

    // write side — if on_response_chunk is set, stream body directly
    // instead of accumulating; for execute() we accumulate unconditionally.

    void refill_from_initial()
    {
        if (initial_chunk_sent)
            return;
        initial_chunk_sent = true;
        if (!request.initial_body_chunk.empty()) {
            body_buffer = request.initial_body_chunk;
            body_offset = 0;
        }
    }

    std::string_view next_read_chunk()
    {
        // Serve remaining bytes in body_buffer first.
        if (body_offset < body_buffer.size()) {
            const char *p = body_buffer.data() + body_offset;
            const size_t remain = body_buffer.size() - body_offset;
            return std::string_view{ p, remain };
        }
        // Ask the user callback.
        if (request.read_body) {
            std::string_view chunk = request.read_body();
            if (!chunk.empty()) {
                body_buffer = std::string{ chunk };
                body_offset = 0;
                return std::string_view{ body_buffer.data(), body_buffer.size() };
            }
        }
        return {}; // EOF
    }

    void consume(size_t n)
    {
        body_offset += n;
        if (body_offset >= body_buffer.size()) {
            body_buffer.clear();
            body_offset = 0;
        }
    }
};

// ---------------------------------------------------------------------------
// curl callback trampolines
// ---------------------------------------------------------------------------

size_t readfn(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *ctx = static_cast<RequestContext *>(userdata);
    const size_t max_bytes = size * nmemb;
    if (max_bytes == 0)
        return 0;

    // First call: seed from initial_body_chunk.
    ctx->refill_from_initial();

    std::string_view chunk = ctx->next_read_chunk();
    if (chunk.empty())
        return 0; // EOF

    const size_t n = std::min(chunk.size(), max_bytes);
    std::memcpy(ptr, chunk.data(), n);
    ctx->consume(n);
    return n;
}

size_t writefn(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *ctx = static_cast<RequestContext *>(userdata);
    const size_t n = size * nmemb;
    if (n == 0)
        return 0;

    // Call optional per-chunk hook.  Returning false aborts the transfer.
    if (ctx->request.on_response_chunk) {
        if (!ctx->request.on_response_chunk(std::string_view{ ptr, n }))
            return 0; // signals error to curl
    }

    ctx->response.body.append(ptr, n);
    return n;
}

size_t headerfn(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *ctx = static_cast<RequestContext *>(userdata);
    const size_t n = size * nmemb;
    std::string_view line{ ptr, n };
    strip_crlf(line);

    // Blank line: end of headers.
    if (line.empty()) {
        if (ctx->request.on_headers && ctx->response.status_code > 0) {
            ctx->request.on_headers(ctx->response.status_code, ctx->response.headers);
        }
        return n;
    }

    // Status line (only the first non-empty line).
    if (ctx->response.status_code == 0) {
        const int code = parse_status_code(line);
        if (code > 0) {
            ctx->response.status_code = code;
            return n;
        }
    }

    // Header line.
    auto [name, value] = parse_header_line(line);
    if (!name.empty())
        ctx->response.headers.push_back({ std::move(name), std::move(value) });

    return n;
}

// ---------------------------------------------------------------------------
// streaming state — thread-safe bridge between curl worker and consumer
// ---------------------------------------------------------------------------

struct StreamState {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> chunks;
    size_t chunk_offset = 0;

    bool headers_ready = false;
    bool worker_done = false;
    bool worker_error = false;
    bool stream_closed = false;

    int status_code = 0;
    std::vector<CurlHeader> headers;

    CURL *easy = nullptr; // owned by worker thread
    std::thread worker;

    ~StreamState()
    {
        if (!worker.joinable())
            return;
        if (worker.get_id() == std::this_thread::get_id()) {
            worker.detach();
            return;
        }
        worker.join();
    }

    StreamState() = default;
    StreamState(const StreamState &) = delete;
    StreamState &operator=(const StreamState &) = delete;
};

size_t stream_writefn(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *state = static_cast<StreamState *>(userdata);
    const size_t n = size * nmemb;
    if (n == 0)
        return 0;
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->chunks.emplace_back(ptr, n);
        state->cv.notify_all();
    }
    return n;
}

size_t stream_headerfn(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *state = static_cast<StreamState *>(userdata);
    const size_t n = size * nmemb;
    std::string_view line{ ptr, n };
    strip_crlf(line);

    if (line.empty()) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->headers_ready = true;
        state->cv.notify_all();
        return n;
    }

    if (state->status_code == 0) {
        const int code = parse_status_code(line);
        if (code > 0) {
            state->status_code = code;
            return n;
        }
    }

    auto [name, value] = parse_header_line(line);
    if (!name.empty())
        state->headers.push_back({ std::move(name), std::move(value) });

    return n;
}

size_t stream_readfn(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *ctx = static_cast<RequestContext *>(userdata);
    const size_t max_bytes = size * nmemb;
    if (max_bytes == 0)
        return 0;

    ctx->refill_from_initial();

    std::string_view chunk = ctx->next_read_chunk();
    if (chunk.empty())
        return 0; // EOF

    const size_t n = std::min(chunk.size(), max_bytes);
    std::memcpy(ptr, chunk.data(), n);
    ctx->consume(n);
    return n;
}

// ---------------------------------------------------------------------------
// worker thread for streaming
// ---------------------------------------------------------------------------

void stream_worker(std::shared_ptr<StreamState> state)
{
    CURLM *multi = curl_multi_init();
    if (!multi) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->worker_error = true;
        state->worker_done = true;
        state->cv.notify_all();
        return;
    }

    curl_multi_add_handle(multi, state->easy);

    int running = 0;
    bool aborted = false;

    do {
        CURLMcode mc = curl_multi_perform(multi, &running);
        if (mc != CURLM_OK) {
            aborted = true;
            break;
        }

        {
            std::lock_guard<std::mutex> lock(state->mu);
            if (state->stream_closed) {
                aborted = true;
                break;
            }
        }

        if (running > 0) {
            curl_multi_wait(multi, nullptr, 0, 100, nullptr);
        }
    } while (running > 0);

    // Drain completion messages.
    int msgs_left = 0;
    CURLMsg *msg = nullptr;
    while ((msg = curl_multi_info_read(multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            if (msg->data.result != CURLE_OK && !aborted) {
                std::lock_guard<std::mutex> lock(state->mu);
                state->worker_error = true;
            }
        }
    }

    curl_multi_remove_handle(multi, state->easy);
    curl_easy_cleanup(state->easy);
    state->easy = nullptr;
    curl_multi_cleanup(multi);

    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->worker_done = true;
        state->cv.notify_all();
    }
}

ssize_t stream_read(std::shared_ptr<StreamState> state, char *out, size_t size)
{
    if (!state)
        return -1;

    std::unique_lock<std::mutex> lock(state->mu);
    for (;;) {
        // Serve from the internal chunk queue.
        if (!state->chunks.empty()) {
            while (!state->chunks.empty() && state->chunk_offset >= state->chunks.front().size()) {
                state->chunks.pop_front();
                state->chunk_offset = 0;
            }
            if (!state->chunks.empty()) {
                const std::string &front = state->chunks.front();
                const size_t available = front.size() - state->chunk_offset;
                const size_t n = std::min(size, available);
                std::memcpy(out, front.data() + state->chunk_offset, n);
                state->chunk_offset += n;
                return static_cast<ssize_t>(n);
            }
        }

        if (state->worker_done)
            return 0;
        if (state->worker_error && state->chunks.empty())
            return -1;

        state->cv.wait_for(lock, std::chrono::milliseconds(100));
    }
}

void stream_close(std::shared_ptr<StreamState> state)
{
    if (!state)
        return;

    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->stream_closed = true;
        state->cv.notify_all();
    }

    if (state->worker.joinable()) {
        // Self-join guard (same as upstream.cpp StreamBridgeState).
        if (state->worker.get_id() == std::this_thread::get_id()) {
            state->worker.detach();
            return;
        }
        state->worker.join();
    }
}

// ---------------------------------------------------------------------------
// curl easy setup shared by execute() and execute_stream()
// ---------------------------------------------------------------------------

void setup_common_options(CURL *easy, const CurlRequest &request)
{
    curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());

    // HTTP/2 only — deployment curl has no h3 support.
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2);

    // No signal handling (threaded usage).
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);

    // TLS.
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);

    // Disable transparent decompression — the proxy uses Accept-Encoding: identity.
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, kEmptyAcceptEncoding);

    // Connection reuse (curl_multi caches connections per multi handle).
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);

    // Timeouts.
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, request.connect_timeout_s);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, request.upload_low_speed_bytes);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, request.upload_low_speed_time);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, request.total_timeout_s);

    // Method.
    const std::string &method = request.method;
    if (method == "POST") {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
    } else if (method == "GET") {
        curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
}

void setup_body_options(CURL *easy, const CurlRequest &request)
{
    const bool has_read_body = static_cast<bool>(request.read_body);
    const bool has_initial = !request.initial_body_chunk.empty();

    // If we have a body to send, enable upload and set Content-Length when
    // the total size is known (no read_body, initial chunk is the entire body).
    if (has_initial || has_read_body) {
        curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);

        if (!has_read_body) {
            // Entire body is known upfront.
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(request.initial_body_chunk.size()));
        }
        // else: chunked transfer encoding (curl handles framing for h2).
    }
}

struct CurlSlistGuard {
    struct curl_slist *list = nullptr;
    ~CurlSlistGuard()
    {
        if (list)
            curl_slist_free_all(list);
    }
    CurlSlistGuard() = default;
    CurlSlistGuard(const CurlSlistGuard &) = delete;
    CurlSlistGuard &operator=(const CurlSlistGuard &) = delete;
};

void setup_headers(CURL *easy, const CurlRequest &request, CurlSlistGuard &guard)
{
    for (const CurlHeader &header : request.headers) {
        // Let curl manage these internally.
        if (header.name == "Host" || header.name == "Content-Length" || header.name == "Connection")
            continue;
        std::string line = header.name + ": " + header.value;
        guard.list = curl_slist_append(guard.list, line.c_str());
    }
    if (guard.list)
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, guard.list);
}

// ---------------------------------------------------------------------------
// multi-handle event loop (non-streaming)
// ---------------------------------------------------------------------------

void drive_multi_to_completion(CURLM *multi, CURL *easy)
{
    int running = 0;
    do {
        CURLMcode mc = curl_multi_perform(multi, &running);
        if (mc != CURLM_OK)
            throw std::runtime_error("curl_multi_perform failed");

        if (running > 0) {
            mc = curl_multi_wait(multi, nullptr, 0, 100, nullptr);
            if (mc != CURLM_OK)
                throw std::runtime_error("curl_multi_wait failed");
        }
    } while (running > 0);

    // Check completion status.
    int msgs_left = 0;
    CURLMsg *msg = curl_multi_info_read(multi, &msgs_left);
    if (msg && msg->msg == CURLMSG_DONE) {
        if (msg->data.result != CURLE_OK) {
            throw std::runtime_error(std::string{ "upstream request failed: " } + curl_easy_strerror(msg->data.result));
        }
    }
}

} // namespace

// ============================================================================
// CurlMultiPool::Impl
// ============================================================================

struct CurlMultiPool::Impl {
    CURLM *multi = nullptr;

    Impl()
        : multi(curl_multi_init())
    {
        if (!multi)
            throw std::runtime_error("curl_multi_init failed");
    }

    ~Impl()
    {
        if (multi)
            curl_multi_cleanup(multi);
    }

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
};

// ============================================================================
// CurlMultiPool
// ============================================================================

CurlMultiPool::CurlMultiPool()
    : impl_(std::make_unique<Impl>())
{
}

CurlMultiPool::~CurlMultiPool() = default;

CurlResponse CurlMultiPool::execute(const CurlRequest &request)
{
    CURL *easy = curl_easy_init();
    if (!easy)
        throw std::runtime_error("curl_easy_init failed");

    // Ensure cleanup on early exit.
    struct EasyGuard {
        CURL *e = nullptr;
        CURLM *m = nullptr;
        bool removed = false;
        ~EasyGuard()
        {
            if (!e)
                return;
            if (m && !removed)
                curl_multi_remove_handle(m, e);
            curl_easy_cleanup(e);
        }
    } guard{ easy, impl_->multi };

    RequestContext ctx;
    ctx.request = request;

    setup_common_options(easy, request);
    setup_body_options(easy, request);

    // DNS pin: prevent rebinding between SSRF validation and curl's own
    // getaddrinfo call.  Only set when upstream.cpp provides a pin.
    CurlSlistGuard dns_guard;
    if (!request.dns_pin.empty()) {
        dns_guard.list = curl_slist_append(dns_guard.list, request.dns_pin.c_str());
        if (dns_guard.list)
            curl_easy_setopt(easy, CURLOPT_RESOLVE, dns_guard.list);
    }

    CurlSlistGuard header_guard;
    setup_headers(easy, request, header_guard);

    // Callbacks.
    curl_easy_setopt(easy, CURLOPT_READFUNCTION, readfn);
    curl_easy_setopt(easy, CURLOPT_READDATA, &ctx);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writefn);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, headerfn);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &ctx);

    CURLMcode mc = curl_multi_add_handle(impl_->multi, easy);
    if (mc != CURLM_OK)
        throw std::runtime_error("curl_multi_add_handle failed");

    drive_multi_to_completion(impl_->multi, easy);

    // Extract the accumulated response *before* removing + cleaning up easy.
    CurlResponse response = std::move(ctx.response);

    curl_multi_remove_handle(impl_->multi, easy);
    guard.removed = true;
    guard.e = nullptr; // EasyGuard won't double-free.

    return response;
}

CurlMultiPool::StreamResult CurlMultiPool::execute_stream(const CurlRequest &request)
{
    CURL *easy = curl_easy_init();
    if (!easy)
        throw std::runtime_error("curl_easy_init failed");

    auto state = std::make_shared<StreamState>();
    state->easy = easy;

    setup_common_options(easy, request);
    setup_body_options(easy, request);

    // DNS pin: prevent rebinding between SSRF validation and curl's own
    // getaddrinfo call.  Only set when upstream.cpp provides a pin.
    CurlSlistGuard dns_guard;
    if (!request.dns_pin.empty()) {
        dns_guard.list = curl_slist_append(dns_guard.list, request.dns_pin.c_str());
        if (dns_guard.list)
            curl_easy_setopt(easy, CURLOPT_RESOLVE, dns_guard.list);
    }

    CurlSlistGuard header_guard;
    setup_headers(easy, request, header_guard);

    // Allocate a RequestContext on the heap — the worker thread owns it.
    auto ctx = std::make_unique<RequestContext>();
    ctx->request = request;

    curl_easy_setopt(easy, CURLOPT_READFUNCTION, stream_readfn);
    curl_easy_setopt(easy, CURLOPT_READDATA, ctx.get());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, stream_writefn);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, state.get());
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, stream_headerfn);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, state.get());

    // Transfer ownership of ctx to the worker thread via a shared_ptr.
    auto ctx_shared = std::shared_ptr<RequestContext>(ctx.release());

    state->worker = std::thread([state, ctx_shared]() mutable {
        stream_worker(std::move(state));
        // ctx_shared is released here (after worker finishes curl calls).
    });

    // Wait for response headers (or timeout).
    {
        const int header_timeout_ms = request.total_timeout_s > 0 ? request.total_timeout_s * 1000 : 60000;
        std::unique_lock<std::mutex> lock(state->mu);
        if (!state->cv.wait_for(lock, std::chrono::milliseconds(header_timeout_ms),
                                [&] { return state->headers_ready || state->worker_done; })) {
            // Timeout: close stream and throw.
            lock.unlock();
            {
                std::lock_guard<std::mutex> lk(state->mu);
                state->stream_closed = true;
                state->cv.notify_all();
            }
            if (state->worker.joinable()) {
                state->worker.join();
            }
            throw std::runtime_error("upstream stream headers timeout");
        }
        if (state->worker_error && !state->headers_ready) {
            lock.unlock();
            if (state->worker.joinable()) {
                state->worker.join();
            }
            throw std::runtime_error("upstream stream request failed");
        }
    }

    StreamResult result;
    result.status_code = state->status_code;
    result.headers = state->headers;

    // Drain any body bytes that arrived before headers were ready.
    {
        std::lock_guard<std::mutex> lock(state->mu);
        if (!state->chunks.empty()) {
            for (const auto &chunk : state->chunks)
                result.initial_body += chunk;
            state->chunks.clear();
            state->chunk_offset = 0;
        }
    }

    result.stream_read = [state](char *out, size_t size) -> ssize_t { return stream_read(state, out, size); };

    result.stream_close = [state]() { stream_close(state); };

    result.poll_fd = -1;

    return result;
}

int CurlMultiPool::perform()
{
    if (!impl_->multi)
        return 0;
    int running = 0;
    CURLMcode mc = curl_multi_perform(impl_->multi, &running);
    if (mc != CURLM_OK)
        return 0;
    return running;
}

} // namespace revlm
