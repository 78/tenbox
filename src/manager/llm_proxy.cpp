// LLM API Proxy Service: HTTP/1.1 server that receives OpenAI-compatible
// requests from the VM (via guestfwd), maps model aliases to real endpoints,
// and forwards using WinHTTP with SSE streaming support.

#include "manager/llm_proxy.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cinttypes>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <share.h>
#include <string>
#include <string_view>
#include <vector>

extern FILE* GetManagerLogFile();
#define LOG_INFO(fmt, ...) do { \
    if (FILE* _f = GetManagerLogFile()) { \
        fprintf(_f, "[llm-proxy] " fmt "\r\n", ##__VA_ARGS__); fflush(_f); \
    } \
} while (0)
#define LOG_ERROR(fmt, ...) do { \
    if (FILE* _f = GetManagerLogFile()) { \
        fprintf(_f, "[llm-proxy][ERROR] " fmt "\r\n", ##__VA_ARGS__); fflush(_f); \
    } \
} while (0)

namespace llm_proxy {

using json = nlohmann::json;

static constexpr int kMaxHeaderSize = 16 * 1024;
static constexpr int kReadBufSize = 8192;

// ── Helpers ──────────────────────────────────────────────────────────

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring r(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, r.data(), len);
    return r;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool is_https = true;
};

static bool ParseUrl(const std::string& url, UrlParts& parts) {
    std::wstring wurl = Utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host_buf[256]{};
    wchar_t path_buf[2048]{};
    uc.lpszHostName = host_buf;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path_buf;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    parts.host = host_buf;
    parts.path = path_buf;
    parts.port = uc.nPort;
    parts.is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

static bool RecvAll(SOCKET sock, char* buf, int len) {
    int received = 0;
    while (received < len) {
        int n = recv(sock, buf + received, len - received, 0);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

static bool SendAll(SOCKET sock, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool SendStr(SOCKET sock, const std::string& s) {
    return SendAll(sock, s.data(), static_cast<int>(s.size()));
}

static std::string ToHex(size_t v) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%zx", v);
    return buf;
}

// Translate WinHTTP / WinInet error codes into short diagnostic strings.
// Only covers codes that are realistic during an SSE stream.
static const char* WinHttpErrorName(DWORD err) {
    switch (err) {
        case 0:                                    return "OK";
        case 12002: /* ERROR_WINHTTP_TIMEOUT */    return "WINHTTP_TIMEOUT";
        case 12004: /* ERROR_WINHTTP_INTERNAL_ERROR */ return "WINHTTP_INTERNAL_ERROR";
        case 12005: /* ERROR_WINHTTP_INVALID_URL */ return "WINHTTP_INVALID_URL";
        case 12007: /* ERROR_WINHTTP_NAME_NOT_RESOLVED */ return "NAME_NOT_RESOLVED";
        case 12029: /* ERROR_WINHTTP_CANNOT_CONNECT */ return "CANNOT_CONNECT";
        case 12030: /* ERROR_WINHTTP_CONNECTION_ERROR */ return "CONNECTION_ERROR";
        case 12031: /* ERROR_WINHTTP_RESET */      return "CONNECTION_RESET";
        case 12032: /* ERROR_WINHTTP_REDIRECT_FAILED */ return "REDIRECT_FAILED";
        case 12152: /* ERROR_WINHTTP_INVALID_SERVER_RESPONSE */ return "INVALID_SERVER_RESPONSE";
        case 12175: /* ERROR_WINHTTP_SECURE_FAILURE */ return "SECURE_FAILURE";
        case 12178: /* ERROR_WINHTTP_AUTODETECTION_FAILED */ return "AUTODETECTION_FAILED";
        default:                                   return "WINHTTP_UNKNOWN";
    }
}

// Translate common Winsock error codes encountered while writing back to
// the VM-side HTTP client.
static const char* WsaErrorName(int err) {
    switch (err) {
        case 0:                     return "OK";
        case WSAEINTR:              return "WSAEINTR";
        case WSAECONNRESET:         return "WSAECONNRESET";
        case WSAECONNABORTED:       return "WSAECONNABORTED";
        case WSAESHUTDOWN:          return "WSAESHUTDOWN";
        case WSAENETRESET:          return "WSAENETRESET";
        case WSAETIMEDOUT:          return "WSAETIMEDOUT";
        case WSAENOTCONN:           return "WSAENOTCONN";
        case WSAENOTSOCK:           return "WSAENOTSOCK";
        default:                    return "WSA_UNKNOWN";
    }
}

static uint64_t NowMonotonicMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ── LlmProxyService ─────────────────────────────────────────────────

LlmProxyService::LlmProxyService(const settings::LlmProxySettings& initial_settings)
    : settings_(initial_settings) {}

LlmProxyService::~LlmProxyService() {
    Stop();
    CloseLogFile();
}

bool LlmProxyService::Start() {
    if (running_) return true;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        LOG_ERROR("Failed to create listen socket: %d", WSAGetLastError());
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // OS assigns random port

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("bind failed: %d", WSAGetLastError());
        closesocket(s);
        return false;
    }

    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bound_len);
    port_.store(ntohs(bound.sin_port));

    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        LOG_ERROR("listen failed: %d", WSAGetLastError());
        closesocket(s);
        return false;
    }

    listen_sock_ = static_cast<uintptr_t>(s);
    running_ = true;
    server_thread_ = std::thread(&LlmProxyService::ServerThread, this);

    LOG_INFO("LLM proxy started on 127.0.0.1:%u", port_.load());

    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        if (settings_.enable_logging)
            OpenLogFile();
    }

    return true;
}

void LlmProxyService::Stop() {
    if (!running_) return;
    running_ = false;

    if (listen_sock_ != ~(uintptr_t)0) {
        closesocket(static_cast<SOCKET>(listen_sock_));
        listen_sock_ = ~(uintptr_t)0;
    }

    if (server_thread_.joinable())
        server_thread_.join();

    port_ = 0;
    LOG_INFO("LLM proxy stopped");
}

void LlmProxyService::UpdateSettings(const settings::LlmProxySettings& settings) {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    bool was_logging = settings_.enable_logging;
    settings_ = settings;

    if (settings.enable_logging && !was_logging) {
        OpenLogFile();
        LOG_INFO("LLM request logging enabled -> %s", log_dir_.c_str());
    } else if (!settings.enable_logging && was_logging) {
        LOG_INFO("LLM request logging disabled");
        CloseLogFile();
    }
}

void LlmProxyService::ServerThread() {
    while (running_) {
        SOCKET client = accept(static_cast<SOCKET>(listen_sock_), nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }

        // Disable Nagle for lower latency (important for SSE streaming)
        int flag = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&flag), sizeof(flag));

        // Handle in-line (single-threaded per connection, multiple VMs
        // are handled via HTTP/1.1 keep-alive and new accept() calls).
        // For simplicity, spawn a thread per connection.
        std::thread([this, client]() {
            HandleClient(static_cast<uintptr_t>(client));
        }).detach();
    }
}

void LlmProxyService::HandleClient(uintptr_t client_sock) {
    SOCKET sock = static_cast<SOCKET>(client_sock);
    bool keep_alive = true;

    while (keep_alive && running_) {
        std::string method, path, body;
        if (!ParseHttpRequest(client_sock, method, path, body, keep_alive))
            break;

        // Route
        if (method == "POST" && (path == "/v1/chat/completions" ||
                                  path == "/chat/completions")) {
            HandleChatCompletions(client_sock, body, keep_alive);
        } else if (method == "GET" && (path == "/v1/models" || path == "/models")) {
            HandleModels(client_sock, keep_alive);
        } else {
            SendErrorResponse(client_sock, 404, "Not Found",
                              "Unknown endpoint: " + path, keep_alive);
        }
    }
    closesocket(sock);
}

bool LlmProxyService::ParseHttpRequest(uintptr_t sock_u, std::string& method,
                                         std::string& path, std::string& body,
                                         bool& keep_alive) {
    SOCKET sock = static_cast<SOCKET>(sock_u);
    std::string header_buf;
    header_buf.reserve(4096);

    // Read headers until \r\n\r\n
    char c;
    while (header_buf.size() < static_cast<size_t>(kMaxHeaderSize)) {
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) return false;
        header_buf.push_back(c);
        if (header_buf.size() >= 4 &&
            header_buf.substr(header_buf.size() - 4) == "\r\n\r\n")
            break;
    }

    // Parse request line
    auto line_end = header_buf.find("\r\n");
    if (line_end == std::string::npos) return false;
    std::string request_line = header_buf.substr(0, line_end);

    auto sp1 = request_line.find(' ');
    auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    method = request_line.substr(0, sp1);
    path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Parse headers
    int content_length = 0;
    keep_alive = true; // HTTP/1.1 default
    std::string headers_section = header_buf.substr(line_end + 2);

    auto find_header = [&](const std::string& name) -> std::string {
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        size_t pos = 0;
        while (pos < headers_section.size()) {
            auto le = headers_section.find("\r\n", pos);
            if (le == std::string::npos || le == pos) break;
            std::string line = headers_section.substr(pos, le - pos);
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                if (key == lower_name) {
                    std::string val = line.substr(colon + 1);
                    while (!val.empty() && val[0] == ' ') val.erase(0, 1);
                    return val;
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

    std::string conn = find_header("connection");
    std::transform(conn.begin(), conn.end(), conn.begin(), ::tolower);
    if (conn == "close") keep_alive = false;

    // Read body
    if (content_length > 0 && content_length < 10 * 1024 * 1024) {
        body.resize(content_length);
        if (!RecvAll(sock, body.data(), content_length)) return false;
    }

    return true;
}

void LlmProxyService::HandleChatCompletions(uintptr_t sock, const std::string& body,
                                              bool keep_alive) {
    json req;
    try {
        req = json::parse(body, nullptr, false);
    } catch (...) {}
    if (req.is_discarded() || !req.is_object()) {
        SendErrorResponse(sock, 400, "Bad Request", "Invalid JSON body", keep_alive);
        return;
    }

    std::string model_name = req.value("model", "");
    if (model_name.empty()) {
        SendErrorResponse(sock, 400, "Bad Request", "Missing 'model' field", keep_alive);
        return;
    }

    const settings::LlmModelMapping* mapping = FindMapping(model_name);
    if (!mapping) {
        SendErrorResponse(sock, 404, "Not Found",
                          "No mapping configured for model: " + model_name, keep_alive);
        return;
    }

    std::string alias = mapping->alias;
    std::string target_model = mapping->model;
    bool is_streaming = req.value("stream", false);

    req["model"] = target_model;
    std::string modified_body = req.dump();

    int upstream_status = 0;
    std::string upstream_response;
    StreamEndInfo stream_end;
    ForwardToUpstream(sock, *mapping, modified_body, is_streaming, keep_alive,
                      upstream_status, upstream_response, stream_end);

    WriteLogEntry(body, upstream_response, alias, target_model,
                  is_streaming, upstream_status, stream_end);
}

void LlmProxyService::HandleModels(uintptr_t sock, bool keep_alive) {
    json models_list = json::array();
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        for (const auto& m : settings_.mappings) {
            models_list.push_back({
                {"id", m.alias},
                {"object", "model"},
                {"owned_by", "tenbox-proxy"}
            });
        }
    }

    json response = {
        {"object", "list"},
        {"data", models_list}
    };

    SendResponse(sock, 200, "OK", "application/json", response.dump(), keep_alive);
}

void LlmProxyService::SendErrorResponse(uintptr_t sock, int status, const char* status_text,
                                          const std::string& message, bool keep_alive) {
    json err = {
        {"error", {
            {"message", message},
            {"type", "proxy_error"},
            {"code", status}
        }}
    };
    SendResponse(sock, status, status_text, "application/json", err.dump(), keep_alive);
}

void LlmProxyService::SendResponse(uintptr_t sock, int status, const char* status_text,
                                     const std::string& content_type, const std::string& body,
                                     bool keep_alive) {
    SOCKET s = static_cast<SOCKET>(sock);
    std::string header = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n"
                         "Content-Type: " + content_type + "\r\n"
                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
                         "Connection: " + (keep_alive ? "keep-alive" : "close") + "\r\n"
                         "\r\n";
    SendStr(s, header);
    if (!body.empty()) SendStr(s, body);
}

const settings::LlmModelMapping* LlmProxyService::FindMapping(const std::string& model_name) const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    for (const auto& m : settings_.mappings) {
        if (m.alias == model_name) return &m;
    }
    return nullptr;
}

bool LlmProxyService::ForwardToUpstream(uintptr_t client_sock,
                                          const settings::LlmModelMapping& mapping,
                                          const std::string& modified_body,
                                          bool is_streaming, bool keep_alive,
                                          int& out_status, std::string& out_response_body,
                                          StreamEndInfo& out_stream_end) {
    SOCKET client = static_cast<SOCKET>(client_sock);
    const uint64_t t_start = NowMonotonicMs();

    // Default classification: becomes "n/a" for non-streaming, otherwise
    // overwritten by the SSE loop below.
    out_stream_end.reason = is_streaming ? "unknown" : "n/a";

    auto set_handshake_error = [&](const std::string& msg, uint32_t err = 0) {
        out_stream_end.reason = "handshake_error";
        out_stream_end.detail = msg;
        out_stream_end.winhttp_error = err;
        out_stream_end.duration_ms = NowMonotonicMs() - t_start;
    };

    // Build upstream URL: target_url + /chat/completions
    std::string upstream_url = mapping.target_url;
    // Ensure no trailing slash duplication
    if (!upstream_url.empty() && upstream_url.back() == '/')
        upstream_url.pop_back();
    upstream_url += "/chat/completions";

    UrlParts parts;
    if (!ParseUrl(upstream_url, parts)) {
        set_handshake_error("invalid_upstream_url");
        SendErrorResponse(client_sock, 502, "Bad Gateway",
                          "Invalid upstream URL: " + mapping.target_url, keep_alive);
        return false;
    }

    HINTERNET session = WinHttpOpen(L"TenBox-LLM-Proxy/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        set_handshake_error("WinHttpOpen failed", GetLastError());
        SendErrorResponse(client_sock, 502, "Bad Gateway",
                          "WinHttpOpen failed", keep_alive);
        return false;
    }

    // Set explicit WinHTTP timeouts. Default receive timeout (30s) is too short for SSE streams.
    if (!WinHttpSetTimeouts(session, 15000, 60000, 30000, 300000)) {
        DWORD err = GetLastError();
        LOG_ERROR("WinHttpSetTimeouts failed: %lu", err);
    }

    // Enable HTTP/1.1 keep-alive (default in WinHTTP)
    HINTERNET conn = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!conn) {
        DWORD err = GetLastError();
        set_handshake_error("WinHttpConnect failed", err);
        WinHttpCloseHandle(session);
        SendErrorResponse(client_sock, 502, "Bad Gateway",
                          "WinHttpConnect failed", keep_alive);
        return false;
    }

    DWORD flags = parts.is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", parts.path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        DWORD err = GetLastError();
        set_handshake_error("WinHttpOpenRequest failed", err);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        SendErrorResponse(client_sock, 502, "Bad Gateway",
                          "WinHttpOpenRequest failed", keep_alive);
        return false;
    }

    // Set headers
    std::wstring headers = L"Content-Type: application/json\r\n";
    headers += L"Authorization: Bearer " + Utf8ToWide(mapping.api_key) + L"\r\n";
    if (is_streaming)
        headers += L"Accept: text/event-stream\r\n";

    BOOL sent = WinHttpSendRequest(req, headers.c_str(),
                                    static_cast<DWORD>(headers.size()),
                                    const_cast<char*>(modified_body.data()),
                                    static_cast<DWORD>(modified_body.size()),
                                    static_cast<DWORD>(modified_body.size()), 0);
    if (!sent) {
        DWORD err = GetLastError();
        set_handshake_error("WinHttpSendRequest failed", err);
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        SendErrorResponse(client_sock, 502, "Bad Gateway",
                          "WinHttpSendRequest failed: " + std::to_string(err), keep_alive);
        return false;
    }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        DWORD err = GetLastError();
        set_handshake_error("WinHttpReceiveResponse failed", err);
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        SendErrorResponse(client_sock, 502, "Bad Gateway",
                          "WinHttpReceiveResponse failed: " + std::to_string(err), keep_alive);
        return false;
    }

    // Get status code
    DWORD status_code = 0;
    DWORD sz = sizeof(status_code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    out_status = static_cast<int>(status_code);

    // Get content-type
    wchar_t ct_buf[256]{};
    DWORD ct_sz = sizeof(ct_buf);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE,
                        WINHTTP_HEADER_NAME_BY_INDEX, ct_buf, &ct_sz,
                        WINHTTP_NO_HEADER_INDEX);

    bool upstream_is_sse = false;
    {
        std::wstring ct_lower(ct_buf);
        std::transform(ct_lower.begin(), ct_lower.end(), ct_lower.begin(), ::towlower);
        upstream_is_sse = (ct_lower.find(L"text/event-stream") != std::wstring::npos);
    }

    if (upstream_is_sse) {
        // SSE streaming: send chunked response
        std::string resp_header =
            "HTTP/1.1 " + std::to_string(status_code) + " OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n"
            "\r\n";
        if (!SendStr(client, resp_header)) {
            out_stream_end.reason = "client_disconnect";
            out_stream_end.detail = "send headers failed";
            out_stream_end.wsa_error = static_cast<uint32_t>(WSAGetLastError());
            out_stream_end.duration_ms = NowMonotonicMs() - t_start;
            goto cleanup;
        }

        // Read SSE data from WinHTTP and forward as chunked encoding.
        // We track who closes first (upstream vs VM-side client) plus a few
        // stats so the jsonl log can explain 60s-style disconnects.
        {
            std::string sse_raw;
            char buf[kReadBufSize];
            DWORD bytes_read = 0;
            BOOL read_result = TRUE;
            uint64_t chunk_count = 0;
            uint64_t bytes_total = 0;
            uint64_t last_chunk_ms = t_start;
            bool saw_done = false;
            bool client_broke = false;
            uint32_t client_err = 0;

            while ((read_result = WinHttpReadData(req, buf, kReadBufSize, &bytes_read)) && bytes_read > 0) {
                sse_raw.append(buf, bytes_read);
                bytes_total += bytes_read;
                ++chunk_count;
                last_chunk_ms = NowMonotonicMs();

                // Cheap scan for [DONE] marker so we can tell "upstream finished
                // gracefully" from "upstream hung up mid-stream".
                if (!saw_done && std::string_view(buf, bytes_read).find("[DONE]") != std::string_view::npos)
                    saw_done = true;

                std::string chunk_header = ToHex(bytes_read) + "\r\n";
                if (!SendStr(client, chunk_header) ||
                    !SendAll(client, buf, static_cast<int>(bytes_read)) ||
                    !SendStr(client, "\r\n")) {
                    client_broke = true;
                    client_err = static_cast<uint32_t>(WSAGetLastError());
                    break;
                }
                bytes_read = 0;
            }
            DWORD last_err = read_result ? 0 : GetLastError();
            uint64_t elapsed_ms = NowMonotonicMs() - t_start;
            uint64_t gap_ms = NowMonotonicMs() - last_chunk_ms;

            out_stream_end.duration_ms = elapsed_ms;
            out_stream_end.upstream_bytes = bytes_total;
            out_stream_end.chunk_count = chunk_count;
            out_stream_end.saw_done_marker = saw_done;

            if (client_broke) {
                // VM-side client closed/reset before upstream finished.
                out_stream_end.reason = "client_disconnect";
                out_stream_end.wsa_error = client_err;
                char detail[256];
                snprintf(detail, sizeof(detail),
                         "VM client closed after %" PRIu64 " ms (%" PRIu64 " bytes, %" PRIu64 " chunks); wsa=%d %s",
                         elapsed_ms,
                         bytes_total,
                         chunk_count,
                         static_cast<int>(client_err),
                         WsaErrorName(static_cast<int>(client_err)));
                out_stream_end.detail = detail;
                LOG_ERROR("%s", detail);
                keep_alive = false;
                // No chunked terminator: upstream may still be streaming.
            } else if (read_result) {
                // Normal end: upstream closed the connection, usually after [DONE].
                out_stream_end.reason = saw_done ? "upstream_done" : "upstream_closed_no_done";
                char detail[256];
                snprintf(detail, sizeof(detail),
                         "upstream %s after %" PRIu64 " ms (%" PRIu64 " bytes, %" PRIu64 " chunks)",
                         saw_done ? "sent [DONE]" : "closed without [DONE]",
                         elapsed_ms,
                         bytes_total,
                         chunk_count);
                out_stream_end.detail = detail;
                // "Closed without [DONE]" is abnormal; surface it as error.
                // The happy path ([DONE]) is intentionally not logged.
                if (!saw_done) {
                    LOG_ERROR("%s", detail);
                }
                SendStr(client, "0\r\n\r\n");
            } else {
                // Abnormal upstream read (timeout, reset, etc.).
                out_stream_end.reason = "upstream_error";
                out_stream_end.winhttp_error = last_err;
                char detail[320];
                snprintf(detail, sizeof(detail),
                         "upstream read failed after %" PRIu64 " ms (%" PRIu64 " bytes, %" PRIu64 " chunks, "
                         "gap=%" PRIu64 " ms, done=%s); winhttp=%lu %s",
                         elapsed_ms,
                         bytes_total,
                         chunk_count,
                         gap_ms,
                         saw_done ? "true" : "false",
                         static_cast<unsigned long>(last_err),
                         WinHttpErrorName(last_err));
                out_stream_end.detail = detail;
                LOG_ERROR("%s", detail);
                keep_alive = false;
                // Propagate the abrupt end to VM-side client.
                shutdown(client, SD_SEND);
            }
            out_response_body = std::move(sse_raw);
        }
    } else {
        // Non-streaming: read full body and send
        std::vector<char> response_body;
        char buf[kReadBufSize];
        DWORD bytes_read = 0;
        while (WinHttpReadData(req, buf, kReadBufSize, &bytes_read) && bytes_read > 0) {
            response_body.insert(response_body.end(), buf, buf + bytes_read);
            bytes_read = 0;
        }

        out_response_body.assign(response_body.begin(), response_body.end());

        std::string resp =
            "HTTP/1.1 " + std::to_string(status_code) + " OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(response_body.size()) + "\r\n"
            "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n"
            "\r\n";
        SendStr(client, resp);
        if (!response_body.empty())
            SendAll(client, response_body.data(), static_cast<int>(response_body.size()));
    }

cleanup:
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return true;
}

// ── Log file helpers ─────────────────────────────────────────────────

namespace fs = std::filesystem;

static std::string TodayDateStr() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_s(&tm_buf, &tt);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
    return buf;
}

static std::string IsoTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    struct tm tm_buf;
    localtime_s(&tm_buf, &tt);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             static_cast<int>(ms.count()));
    return buf;
}

// Extract assistant content from SSE stream (concatenate all delta content pieces).
static std::string ExtractSseContent(const std::string& sse_raw) {
    std::string content;
    size_t pos = 0;
    while (pos < sse_raw.size()) {
        auto line_end = sse_raw.find('\n', pos);
        if (line_end == std::string::npos) line_end = sse_raw.size();
        std::string line = sse_raw.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = line_end + 1;

        if (line.rfind("data: ", 0) != 0) continue;
        auto data = line.substr(6);
        if (data == "[DONE]") break;

        try {
            auto j = json::parse(data, nullptr, false);
            if (j.is_discarded()) continue;
            if (j.contains("choices") && j["choices"].is_array()) {
                for (auto& choice : j["choices"]) {
                    if (choice.contains("delta") && choice["delta"].contains("content")) {
                        content += choice["delta"]["content"].get<std::string>();
                    }
                }
            }
        } catch (...) {}
    }
    return content;
}

std::string LlmProxyService::GetLogDir() const {
    wchar_t path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        return (fs::path(path) / "TenBox" / "llm_logs").string();
    }
    return {};
}

void LlmProxyService::OpenLogFile() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (log_file_) return;

    log_dir_ = GetLogDir();
    if (log_dir_.empty()) return;

    std::error_code ec;
    fs::create_directories(log_dir_, ec);
    if (ec) return;

    current_log_date_ = TodayDateStr();
    auto path = (fs::path(log_dir_) / ("llm_" + current_log_date_ + ".jsonl")).string();
    log_file_ = _fsopen(path.c_str(), "ab", _SH_DENYWR);
}

void LlmProxyService::CloseLogFile() {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (log_file_) {
        fclose(log_file_);
        log_file_ = nullptr;
    }
    current_log_date_.clear();
}

void LlmProxyService::WriteLogEntry(const std::string& request_body,
                                     const std::string& response_body,
                                     const std::string& alias,
                                     const std::string& model,
                                     bool is_streaming, int status_code,
                                     const StreamEndInfo& stream_end) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (!log_file_) return;

    auto today = TodayDateStr();
    if (today != current_log_date_) {
        fclose(log_file_);
        current_log_date_ = today;
        auto path = (fs::path(log_dir_) / ("llm_" + today + ".jsonl")).string();
        log_file_ = _fsopen(path.c_str(), "ab", _SH_DENYWR);
        if (!log_file_) return;
    }

    json entry;
    entry["timestamp"] = IsoTimestamp();
    entry["alias"] = alias;
    entry["model"] = model;
    entry["stream"] = is_streaming;
    entry["status"] = status_code;

    // Diagnostics: who closed the connection and why. Always present so log
    // readers can filter on stream_end.reason != "upstream_done".
    json end = {
        {"reason", stream_end.reason},
        {"duration_ms", stream_end.duration_ms},
    };
    if (!stream_end.detail.empty())       end["detail"]         = stream_end.detail;
    if (stream_end.winhttp_error)         end["winhttp_error"]  = stream_end.winhttp_error;
    if (stream_end.wsa_error)             end["wsa_error"]      = stream_end.wsa_error;
    if (is_streaming) {
        end["upstream_bytes"]   = stream_end.upstream_bytes;
        end["chunk_count"]      = stream_end.chunk_count;
        end["saw_done_marker"]  = stream_end.saw_done_marker;
    }
    entry["stream_end"] = std::move(end);

    // Parse request to extract messages and params
    try {
        auto req = json::parse(request_body, nullptr, false);
        if (!req.is_discarded() && req.is_object()) {
            if (req.contains("messages")) entry["messages"] = req["messages"];
            if (req.contains("temperature")) entry["temperature"] = req["temperature"];
            if (req.contains("max_tokens")) entry["max_tokens"] = req["max_tokens"];
            if (req.contains("top_p")) entry["top_p"] = req["top_p"];
        }
    } catch (...) {}

    // Extract response content
    if (status_code >= 200 && status_code < 300) {
        if (is_streaming) {
            entry["response"] = ExtractSseContent(response_body);
        } else {
            try {
                auto resp = json::parse(response_body, nullptr, false);
                if (!resp.is_discarded() && resp.is_object()) {
                    if (resp.contains("choices") && resp["choices"].is_array() &&
                        !resp["choices"].empty()) {
                        auto& choice = resp["choices"][0];
                        if (choice.contains("message"))
                            entry["response"] = choice["message"];
                    }
                    if (resp.contains("usage"))
                        entry["usage"] = resp["usage"];
                }
            } catch (...) {
                entry["response"] = response_body;
            }
        }
    } else {
        try {
            auto err = json::parse(response_body, nullptr, false);
            if (!err.is_discarded()) entry["error"] = err;
            else entry["error"] = response_body;
        } catch (...) {
            entry["error"] = response_body;
        }
    }

    auto line = entry.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    fwrite(line.data(), 1, line.size(), log_file_);
    fflush(log_file_);
}

}  // namespace llm_proxy
