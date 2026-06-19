// LLM API reverse proxy for agentsphered.
//
// Linux/POSIX port of src/manager/llm_proxy.cpp. The Win32 version uses
// WinHTTP for the upstream call; here we use libcurl with
// CURLOPT_WRITEFUNCTION so SSE chunks are forwarded back to the guest as
// they arrive instead of buffering the whole response.
//
// Reachability: the listen socket binds to 127.0.0.1. QEMU's slirp NAT
// (default user-mode networking) maps 10.0.2.2 inside the guest to the
// host loopback, so guests can reach this proxy at
// http://10.0.2.2:<port>/v1/... without any per-VM guestfwd entry. Hosts
// that disable slirp (TAP / bridged) need an explicit guestfwd; that is
// the operator's responsibility.

#include "daemon/llm_proxy.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace tenbox::daemon {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kMaxHeaderSize = 16 * 1024;
constexpr int kReadBufSize = 8192;
constexpr int kMaxBodySize = 10 * 1024 * 1024;  // 10 MiB request body cap

// Process-wide curl_global_init guard. libcurl insists this runs once
// before any worker thread touches it; calling it more than once is fine
// but wasteful. Triggered the first time any LlmProxyService starts.
std::once_flag g_curl_global_init;

void EnsureCurlGlobalInit() {
    std::call_once(g_curl_global_init, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        // Ignore SIGPIPE process-wide so a guest closing its side of the
        // SSE stream does not kill the daemon. We also use MSG_NOSIGNAL
        // on every send() below; the global mask is belt-and-suspenders
        // for any future code path that forgets.
        ::signal(SIGPIPE, SIG_IGN);
    });
}

#define LOG_INFO(fmt, ...) \
    do { std::fprintf(stderr, "[llm-proxy] " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOG_ERROR(fmt, ...) \
    do { std::fprintf(stderr, "[llm-proxy][ERROR] " fmt "\n", ##__VA_ARGS__); } while (0)

uint64_t NowMonotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool SendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool SendStr(int fd, const std::string& s) {
    return SendAll(fd, s.data(), s.size());
}

bool RecvExact(int fd, char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, buf + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

std::string ToHex(size_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%zx", v);
    return buf;
}

std::string ToLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string TrimLeading(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string TodayDateStr() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&tt, &tm_buf);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
    return buf;
}

std::string IsoTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    struct tm tm_buf;
    localtime_r(&tt, &tm_buf);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

// Walk an SSE buffer and concatenate every choices[].delta.content piece.
// Used by the jsonl logger so the human-readable response field carries
// the actual assistant text instead of raw `data: {...}` lines.
std::string ExtractSseContent(const std::string& sse_raw) {
    std::string content;
    size_t pos = 0;
    while (pos < sse_raw.size()) {
        size_t line_end = sse_raw.find('\n', pos);
        if (line_end == std::string::npos) line_end = sse_raw.size();
        std::string line = sse_raw.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = line_end + 1;

        if (line.rfind("data: ", 0) != 0) continue;
        std::string data = line.substr(6);
        if (data == "[DONE]") break;

        auto j = json::parse(data, nullptr, false);
        if (j.is_discarded()) continue;
        if (j.contains("choices") && j["choices"].is_array()) {
            for (auto& choice : j["choices"]) {
                if (choice.contains("delta") && choice["delta"].contains("content") &&
                    choice["delta"]["content"].is_string()) {
                    content += choice["delta"]["content"].get<std::string>();
                }
            }
        }
    }
    return content;
}

// State carried through the libcurl WRITEFUNCTION callback for one
// upstream chat-completions transfer. Lives on the connection-handling
// thread's stack; no synchronization required.
struct ForwardCallbackContext {
    int client_fd = -1;
    bool is_streaming = false;
    bool headers_sent = false;
    bool client_broke = false;
    int client_errno = 0;
    int upstream_status = 0;
    std::string upstream_content_type;
    bool upstream_is_sse = false;
    bool keep_alive = true;
    // Accumulates the entire upstream payload for the jsonl log; for SSE
    // responses we still keep the raw stream around so the logger can
    // extract assistant content via ExtractSseContent.
    std::string captured_body;
    uint64_t bytes_total = 0;
    uint64_t chunk_count = 0;
    uint64_t last_chunk_ms = 0;
    bool saw_done = false;
};

// libcurl HEADERFUNCTION: gathers status code + content-type from the
// first response line and the Content-Type header so the data callback
// knows whether to switch to chunked SSE forwarding mode.
size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<ForwardCallbackContext*>(userdata);
    size_t total = size * nitems;
    std::string_view line(buffer, total);

    // Strip trailing \r\n for the lower-case match.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }

    // Status line, e.g. "HTTP/1.1 200 OK".
    if (line.rfind("HTTP/", 0) == 0) {
        // Reset on each status line so a 100-continue + 200 sequence ends
        // up reporting the final status.
        ctx->upstream_content_type.clear();
        ctx->upstream_is_sse = false;
        size_t sp1 = line.find(' ');
        if (sp1 != std::string_view::npos) {
            size_t sp2 = line.find(' ', sp1 + 1);
            std::string_view code = line.substr(sp1 + 1,
                (sp2 == std::string_view::npos ? line.size() : sp2) - sp1 - 1);
            int v = 0;
            std::from_chars(code.data(), code.data() + code.size(), v);
            ctx->upstream_status = v;
        }
        return total;
    }

    // Header "Name: value".
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return total;
    std::string name = ToLowerCopy(std::string(line.substr(0, colon)));
    std::string value = TrimLeading(std::string(line.substr(colon + 1)));
    if (name == "content-type") {
        ctx->upstream_content_type = value;
        std::string lower = ToLowerCopy(value);
        ctx->upstream_is_sse = lower.find("text/event-stream") != std::string::npos;
    }
    return total;
}

// libcurl WRITEFUNCTION: for SSE responses, forward each chunk back to
// the guest as an HTTP/1.1 chunked-transfer-encoding chunk. Returning
// less than the bytes received aborts the transfer, which is how we
// propagate "guest hung up" upstream.
size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<ForwardCallbackContext*>(userdata);
    size_t total = size * nmemb;
    if (total == 0) return 0;

    ctx->bytes_total += total;
    ctx->chunk_count += 1;
    ctx->last_chunk_ms = NowMonotonicMs();
    ctx->captured_body.append(ptr, total);
    if (!ctx->saw_done &&
        std::string_view(ptr, total).find("[DONE]") != std::string_view::npos) {
        ctx->saw_done = true;
    }

    if (ctx->is_streaming && ctx->upstream_is_sse) {
        if (!ctx->headers_sent) {
            std::string resp_header =
                "HTTP/1.1 " + std::to_string(ctx->upstream_status) + " OK\r\n"
                "Content-Type: " + (ctx->upstream_content_type.empty()
                                       ? "text/event-stream; charset=utf-8"
                                       : ctx->upstream_content_type) + "\r\n"
                "Cache-Control: no-cache\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Connection: " +
                std::string(ctx->keep_alive ? "keep-alive" : "close") + "\r\n"
                "\r\n";
            if (!SendStr(ctx->client_fd, resp_header)) {
                ctx->client_broke = true;
                ctx->client_errno = errno;
                return 0;
            }
            ctx->headers_sent = true;
        }
        std::string chunk_header = ToHex(total) + "\r\n";
        if (!SendStr(ctx->client_fd, chunk_header) ||
            !SendAll(ctx->client_fd, ptr, total) ||
            !SendStr(ctx->client_fd, "\r\n")) {
            ctx->client_broke = true;
            ctx->client_errno = errno;
            return 0;
        }
    }
    return total;
}

}  // namespace

// ── LlmProxyService ─────────────────────────────────────────────────

LlmProxyService::LlmProxyService(const LlmProxySettings& initial_settings,
                                 std::string data_dir)
    : data_dir_(std::move(data_dir)), settings_(initial_settings) {
    EnsureCurlGlobalInit();
}

LlmProxyService::~LlmProxyService() {
    Stop();
    std::lock_guard<std::mutex> lock(log_mutex_);
    CloseLogFileLocked();
}

bool LlmProxyService::Start() {
    if (running_.load()) return true;

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOG_ERROR("socket() failed: %s", std::strerror(errno));
        return false;
    }

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        addr.sin_port = htons(settings_.listen_port);
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("bind() failed: %s", std::strerror(errno));
        ::close(fd);
        return false;
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        port_.store(ntohs(bound.sin_port));
    }

    if (::listen(fd, SOMAXCONN) < 0) {
        LOG_ERROR("listen() failed: %s", std::strerror(errno));
        ::close(fd);
        port_.store(0);
        return false;
    }

    listen_fd_ = fd;
    running_.store(true);
    server_thread_ = std::thread(&LlmProxyService::ServerThread, this);

    LOG_INFO("started on 127.0.0.1:%u (guests reach as 10.0.2.2:%u)",
             port_.load(), port_.load());

    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (settings_.enable_logging) OpenLogFileLocked();
    }
    return true;
}

void LlmProxyService::Stop() {
    if (!running_.exchange(false)) return;

    if (listen_fd_ >= 0) {
        // shutdown() on the listen socket unblocks accept(); close()
        // alone is racy because accept() may already be parked in the
        // kernel.
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (server_thread_.joinable()) server_thread_.join();
    port_.store(0);
    LOG_INFO("stopped");
}

void LlmProxyService::UpdateSettings(const LlmProxySettings& settings) {
    bool was_logging = false;
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        was_logging = settings_.enable_logging;
        settings_ = settings;
    }
    std::lock_guard<std::mutex> log_lock(log_mutex_);
    if (settings.enable_logging && !was_logging) {
        OpenLogFileLocked();
        LOG_INFO("request logging enabled");
    } else if (!settings.enable_logging && was_logging) {
        LOG_INFO("request logging disabled");
        CloseLogFileLocked();
    }
}

void LlmProxyService::ServerThread() {
    pthread_setname_np(pthread_self(), "llm-listener");
    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client < 0) {
            if (!running_.load()) break;
            if (errno == EINTR) continue;
            LOG_ERROR("accept() failed: %s", std::strerror(errno));
            continue;
        }

        int yes = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        // Per-connection thread. Detach: lifetime is bounded by the
        // socket itself, and Stop() drops the listen socket so no new
        // workers are spawned afterwards.
        std::thread([this, client]() {
            HandleClient(client);
        }).detach();
    }
}

void LlmProxyService::HandleClient(int client_fd) {
    pthread_setname_np(pthread_self(), "llm-handler");
    bool keep_alive = true;
    while (keep_alive && running_.load()) {
        std::string method;
        std::string path;
        std::string body;
        if (!ParseHttpRequest(client_fd, method, path, body, keep_alive)) break;

        if (method == "POST" && (path == "/v1/chat/completions" ||
                                  path == "/chat/completions")) {
            HandleChatCompletions(client_fd, body, keep_alive);
        } else if (method == "GET" && (path == "/v1/models" || path == "/models")) {
            HandleModels(client_fd, keep_alive);
        } else {
            SendErrorResponse(client_fd, 404, "Not Found",
                              "Unknown endpoint: " + path, keep_alive);
        }
    }
    ::close(client_fd);
}

bool LlmProxyService::ParseHttpRequest(int client_fd, std::string& method,
                                        std::string& path, std::string& body,
                                        bool& keep_alive) {
    std::string header_buf;
    header_buf.reserve(4096);

    char c = 0;
    while (header_buf.size() < static_cast<size_t>(kMaxHeaderSize)) {
        ssize_t n = ::recv(client_fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        header_buf.push_back(c);
        if (header_buf.size() >= 4 &&
            header_buf.compare(header_buf.size() - 4, 4, "\r\n\r\n") == 0) {
            break;
        }
    }

    auto line_end = header_buf.find("\r\n");
    if (line_end == std::string::npos) return false;
    std::string request_line = header_buf.substr(0, line_end);

    auto sp1 = request_line.find(' ');
    auto sp2 = (sp1 == std::string::npos) ? std::string::npos
                                          : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    method = request_line.substr(0, sp1);
    path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    int content_length = 0;
    keep_alive = true;
    std::string headers_section = header_buf.substr(line_end + 2);

    auto find_header = [&](const std::string& name) -> std::string {
        std::string target = ToLowerCopy(name);
        size_t pos = 0;
        while (pos < headers_section.size()) {
            auto le = headers_section.find("\r\n", pos);
            if (le == std::string::npos || le == pos) break;
            std::string line = headers_section.substr(pos, le - pos);
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                if (ToLowerCopy(line.substr(0, colon)) == target) {
                    return TrimLeading(line.substr(colon + 1));
                }
            }
            pos = le + 2;
        }
        return {};
    };

    std::string cl = find_header("content-length");
    if (!cl.empty()) {
        std::from_chars(cl.data(), cl.data() + cl.size(), content_length);
    }
    if (ToLowerCopy(find_header("connection")) == "close") keep_alive = false;

    if (content_length > 0 && content_length <= kMaxBodySize) {
        body.resize(static_cast<size_t>(content_length));
        if (!RecvExact(client_fd, body.data(), body.size())) return false;
    } else if (content_length > kMaxBodySize) {
        // Refuse oversized bodies rather than allocating; close the
        // socket to avoid getting stuck reading garbage.
        return false;
    }
    return true;
}

void LlmProxyService::HandleChatCompletions(int client_fd, const std::string& body,
                                             bool keep_alive) {
    auto req = json::parse(body, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        SendErrorResponse(client_fd, 400, "Bad Request", "Invalid JSON body", keep_alive);
        return;
    }

    std::string model_name = req.value("model", "");
    if (model_name.empty()) {
        SendErrorResponse(client_fd, 400, "Bad Request", "Missing 'model' field", keep_alive);
        return;
    }

    bool found = false;
    LlmModelMapping mapping = FindMappingCopy(model_name, found);
    if (!found) {
        SendErrorResponse(client_fd, 404, "Not Found",
                          "No mapping configured for model: " + model_name, keep_alive);
        return;
    }

    bool is_streaming = req.value("stream", false);
    req["model"] = mapping.model;
    std::string modified_body = req.dump();

    int upstream_status = 0;
    std::string upstream_response;
    StreamEndInfo stream_end;
    bool keep_alive_after = keep_alive;
    ForwardToUpstream(client_fd, mapping, modified_body, is_streaming, keep_alive_after,
                      upstream_status, upstream_response, stream_end);
    keep_alive = keep_alive_after;

    WriteLogEntry(body, upstream_response, mapping.alias, mapping.model,
                  is_streaming, upstream_status, stream_end);
}

void LlmProxyService::HandleModels(int client_fd, bool keep_alive) {
    json models_list = json::array();
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        for (const auto& m : settings_.mappings) {
            models_list.push_back({
                {"id", m.alias},
                {"object", "model"},
                {"owned_by", "tenbox-proxy"},
            });
        }
    }
    json response = {
        {"object", "list"},
        {"data", models_list},
    };
    SendResponse(client_fd, 200, "OK", "application/json", response.dump(), keep_alive);
}

LlmModelMapping LlmProxyService::FindMappingCopy(const std::string& alias, bool& found) const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    for (const auto& m : settings_.mappings) {
        if (m.alias == alias) {
            found = true;
            return m;
        }
    }
    found = false;
    return {};
}

bool LlmProxyService::ForwardToUpstream(int client_fd, const LlmModelMapping& mapping,
                                          const std::string& modified_body,
                                          bool is_streaming, bool& keep_alive,
                                          int& out_status, std::string& out_response_body,
                                          StreamEndInfo& out_stream_end) {
    const uint64_t t_start = NowMonotonicMs();
    out_stream_end.reason = is_streaming ? "unknown" : "n/a";

    std::string upstream_url = mapping.target_url;
    if (!upstream_url.empty() && upstream_url.back() == '/') upstream_url.pop_back();
    upstream_url += "/chat/completions";

    CURL* curl = curl_easy_init();
    if (!curl) {
        out_stream_end.reason = "handshake_error";
        out_stream_end.detail = "curl_easy_init failed";
        out_stream_end.duration_ms = NowMonotonicMs() - t_start;
        SendErrorResponse(client_fd, 502, "Bad Gateway",
                          "curl_easy_init failed", keep_alive);
        return false;
    }

    ForwardCallbackContext ctx;
    ctx.client_fd = client_fd;
    ctx.is_streaming = is_streaming;
    ctx.keep_alive = keep_alive;
    ctx.last_chunk_ms = t_start;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!mapping.api_key.empty()) {
        std::string auth = "Authorization: Bearer " + mapping.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }
    if (is_streaming) {
        headers = curl_slist_append(headers, "Accept: text/event-stream");
    }
    // Some upstreams (e.g. Anthropic-via-proxy gateways) aggressively
    // gzip everything; CURL handles decoding transparently when we ask
    // for it explicitly via Accept-Encoding.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    curl_easy_setopt(curl, CURLOPT_URL, upstream_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, modified_body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(modified_body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    // Streaming responses can sit idle between tokens; disable the
    // total-transfer timeout but keep a low-water mark on connection
    // setup. Non-streaming requests inherit a 5 minute hard ceiling so
    // a wedged upstream cannot hold the worker thread forever.
    if (is_streaming) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    }

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    out_status = static_cast<int>(http_code ? http_code : ctx.upstream_status);

    uint64_t elapsed_ms = NowMonotonicMs() - t_start;
    uint64_t gap_ms = NowMonotonicMs() - ctx.last_chunk_ms;
    out_stream_end.duration_ms = elapsed_ms;
    out_stream_end.upstream_bytes = ctx.bytes_total;
    out_stream_end.chunk_count = ctx.chunk_count;
    out_stream_end.saw_done_marker = ctx.saw_done;
    out_response_body = std::move(ctx.captured_body);

    if (ctx.client_broke) {
        out_stream_end.reason = "client_disconnect";
        out_stream_end.client_errno = ctx.client_errno;
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "guest closed after %llu ms (%llu bytes, %llu chunks); errno=%d %s",
                      static_cast<unsigned long long>(elapsed_ms),
                      static_cast<unsigned long long>(ctx.bytes_total),
                      static_cast<unsigned long long>(ctx.chunk_count),
                      ctx.client_errno, std::strerror(ctx.client_errno));
        out_stream_end.detail = detail;
        keep_alive = false;
        // Don't bother emitting a chunked terminator; the guest is gone.
    } else if (rc != CURLE_OK) {
        out_stream_end.reason = "upstream_error";
        out_stream_end.curl_code = static_cast<int>(rc);
        char detail[320];
        std::snprintf(detail, sizeof(detail),
                      "upstream error after %llu ms (%llu bytes, %llu chunks, gap=%llu ms): %s",
                      static_cast<unsigned long long>(elapsed_ms),
                      static_cast<unsigned long long>(ctx.bytes_total),
                      static_cast<unsigned long long>(ctx.chunk_count),
                      static_cast<unsigned long long>(gap_ms),
                      curl_easy_strerror(rc));
        out_stream_end.detail = detail;
        LOG_ERROR("%s", detail);
        if (!ctx.headers_sent) {
            SendErrorResponse(client_fd, 502, "Bad Gateway",
                              std::string("upstream error: ") + curl_easy_strerror(rc),
                              keep_alive);
        } else {
            // SSE header already flushed; we cannot send a clean error
            // response. Half-close so the guest sees EOF.
            ::shutdown(client_fd, SHUT_WR);
            keep_alive = false;
        }
    } else if (is_streaming && ctx.upstream_is_sse) {
        out_stream_end.reason = ctx.saw_done ? "upstream_done" : "upstream_closed_no_done";
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "upstream %s after %llu ms (%llu bytes, %llu chunks)",
                      ctx.saw_done ? "sent [DONE]" : "closed without [DONE]",
                      static_cast<unsigned long long>(elapsed_ms),
                      static_cast<unsigned long long>(ctx.bytes_total),
                      static_cast<unsigned long long>(ctx.chunk_count));
        out_stream_end.detail = detail;
        if (ctx.headers_sent) {
            // Standard chunked-encoding terminator: zero-length chunk.
            SendStr(client_fd, "0\r\n\r\n");
        } else {
            // Upstream sent zero bytes on a 200 OK; surface as empty response.
            SendResponse(client_fd, out_status ? out_status : 200, "OK",
                         ctx.upstream_content_type.empty()
                             ? "text/event-stream; charset=utf-8"
                             : ctx.upstream_content_type,
                         "", keep_alive);
        }
    } else {
        // Non-streaming upstream (or upstream chose not to stream even
        // though we asked). Forward the captured body verbatim with the
        // original content type so the guest sees the exact response.
        out_stream_end.reason = is_streaming ? "upstream_non_sse_response" : "n/a";
        std::string content_type = ctx.upstream_content_type.empty()
                                       ? "application/json"
                                       : ctx.upstream_content_type;
        SendResponse(client_fd, out_status ? out_status : 200, "OK", content_type,
                     out_response_body, keep_alive);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return true;
}

void LlmProxyService::SendErrorResponse(int client_fd, int status, const char* status_text,
                                          const std::string& message, bool keep_alive) {
    json err = {
        {"error", {
            {"message", message},
            {"type", "proxy_error"},
            {"code", status},
        }},
    };
    SendResponse(client_fd, status, status_text, "application/json", err.dump(), keep_alive);
}

void LlmProxyService::SendResponse(int client_fd, int status, const char* status_text,
                                     const std::string& content_type,
                                     const std::string& body, bool keep_alive) {
    std::string header = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n"
                         "Content-Type: " + content_type + "\r\n"
                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
                         "Connection: " + (keep_alive ? "keep-alive" : "close") + "\r\n"
                         "\r\n";
    SendStr(client_fd, header);
    if (!body.empty()) SendAll(client_fd, body.data(), body.size());
}

// ── Logging ──────────────────────────────────────────────────────────

std::string LlmProxyService::LogDir() const {
    return (fs::path(data_dir_) / "llm_logs").string();
}

void LlmProxyService::OpenLogFileLocked() {
    if (log_file_) return;
    auto dir = LogDir();
    if (dir.empty()) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("create_directories(%s) failed: %s", dir.c_str(), ec.message().c_str());
        return;
    }
    current_log_date_ = TodayDateStr();
    auto path = (fs::path(dir) / ("llm_" + current_log_date_ + ".jsonl")).string();
    log_file_ = std::fopen(path.c_str(), "ab");
    if (!log_file_) {
        LOG_ERROR("fopen(%s) failed: %s", path.c_str(), std::strerror(errno));
    }
}

void LlmProxyService::CloseLogFileLocked() {
    if (log_file_) {
        std::fclose(log_file_);
        log_file_ = nullptr;
    }
    current_log_date_.clear();
}

void LlmProxyService::RotateLogFileIfNeededLocked() {
    auto today = TodayDateStr();
    if (today == current_log_date_) return;
    CloseLogFileLocked();
    OpenLogFileLocked();
}

void LlmProxyService::WriteLogEntry(const std::string& request_body,
                                     const std::string& response_body,
                                     const std::string& alias, const std::string& model,
                                     bool is_streaming, int status_code,
                                     const StreamEndInfo& stream_end) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (!log_file_) return;
    RotateLogFileIfNeededLocked();
    if (!log_file_) return;

    json entry;
    entry["timestamp"] = IsoTimestamp();
    entry["alias"] = alias;
    entry["model"] = model;
    entry["stream"] = is_streaming;
    entry["status"] = status_code;

    json end = {
        {"reason", stream_end.reason},
        {"duration_ms", stream_end.duration_ms},
    };
    if (!stream_end.detail.empty()) end["detail"] = stream_end.detail;
    if (stream_end.curl_code) end["curl_code"] = stream_end.curl_code;
    if (stream_end.client_errno) end["client_errno"] = stream_end.client_errno;
    if (is_streaming) {
        end["upstream_bytes"] = stream_end.upstream_bytes;
        end["chunk_count"] = stream_end.chunk_count;
        end["saw_done_marker"] = stream_end.saw_done_marker;
    }
    entry["stream_end"] = std::move(end);

    auto req = json::parse(request_body, nullptr, false);
    if (!req.is_discarded() && req.is_object()) {
        if (req.contains("messages")) entry["messages"] = req["messages"];
        if (req.contains("temperature")) entry["temperature"] = req["temperature"];
        if (req.contains("max_tokens")) entry["max_tokens"] = req["max_tokens"];
        if (req.contains("top_p")) entry["top_p"] = req["top_p"];
    }

    if (status_code >= 200 && status_code < 300) {
        if (is_streaming) {
            entry["response"] = ExtractSseContent(response_body);
        } else {
            auto resp = json::parse(response_body, nullptr, false);
            if (!resp.is_discarded() && resp.is_object()) {
                if (resp.contains("choices") && resp["choices"].is_array() &&
                    !resp["choices"].empty()) {
                    auto& choice = resp["choices"][0];
                    if (choice.contains("message")) entry["response"] = choice["message"];
                }
                if (resp.contains("usage")) entry["usage"] = resp["usage"];
            } else {
                entry["response"] = response_body;
            }
        }
    } else {
        auto err = json::parse(response_body, nullptr, false);
        if (!err.is_discarded()) entry["error"] = err;
        else entry["error"] = response_body;
    }

    auto line = entry.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    std::fwrite(line.data(), 1, line.size(), log_file_);
    std::fflush(log_file_);
}

}  // namespace tenbox::daemon
