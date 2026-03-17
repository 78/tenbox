// LLM API Proxy Service: HTTP/1.1 server that receives OpenAI-compatible
// requests from the VM (via guestfwd), maps model aliases to real endpoints,
// and forwards using WinHTTP with SSE streaming support.

#include "manager/llm_proxy.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>
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

// ── LlmProxyService ─────────────────────────────────────────────────

LlmProxyService::LlmProxyService(const settings::LlmProxySettings& initial_settings)
    : settings_(initial_settings) {}

LlmProxyService::~LlmProxyService() {
    Stop();
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
    settings_ = settings;
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

    bool is_streaming = req.value("stream", false);

    // Replace model name
    req["model"] = mapping->model;
    std::string modified_body = req.dump();

    if (!ForwardToUpstream(sock, *mapping, modified_body, is_streaming, keep_alive)) {
        // Error response already sent by ForwardToUpstream on failure
    }
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
                                          bool is_streaming, bool keep_alive) {
    SOCKET client = static_cast<SOCKET>(client_sock);

    // Build upstream URL: target_url + /chat/completions
    std::string upstream_url = mapping.target_url;
    // Ensure no trailing slash duplication
    if (!upstream_url.empty() && upstream_url.back() == '/')
        upstream_url.pop_back();
    upstream_url += "/chat/completions";

    UrlParts parts;
    if (!ParseUrl(upstream_url, parts)) {
        SendErrorResponse(client_sock, 502, "Bad Gateway",
                          "Invalid upstream URL: " + mapping.target_url, keep_alive);
        return false;
    }

    HINTERNET session = WinHttpOpen(L"TenBox-LLM-Proxy/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        SendErrorResponse(client_sock, 502, "Bad Gateway",
                          "WinHttpOpen failed", keep_alive);
        return false;
    }

    // Enable HTTP/1.1 keep-alive (default in WinHTTP)
    HINTERNET conn = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!conn) {
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
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        SendErrorResponse(client_sock, 502, "Bad Gateway",
                          "WinHttpSendRequest failed: " + std::to_string(err), keep_alive);
        return false;
    }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        DWORD err = GetLastError();
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
        if (!SendStr(client, resp_header)) goto cleanup;

        // Read SSE data from WinHTTP and forward as chunked encoding
        {
            char buf[kReadBufSize];
            DWORD bytes_read = 0;
            while (WinHttpReadData(req, buf, sizeof(buf), &bytes_read) && bytes_read > 0) {
                // Send chunk: size\r\n data\r\n
                std::string chunk_header = ToHex(bytes_read) + "\r\n";
                if (!SendStr(client, chunk_header)) break;
                if (!SendAll(client, buf, static_cast<int>(bytes_read))) break;
                if (!SendStr(client, "\r\n")) break;
                bytes_read = 0;
            }
            // Terminal chunk
            SendStr(client, "0\r\n\r\n");
        }
    } else {
        // Non-streaming: read full body and send
        std::vector<char> response_body;
        char buf[kReadBufSize];
        DWORD bytes_read = 0;
        while (WinHttpReadData(req, buf, sizeof(buf), &bytes_read) && bytes_read > 0) {
            response_body.insert(response_body.end(), buf, buf + bytes_read);
            bytes_read = 0;
        }

        // Build response
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

}  // namespace llm_proxy
