#pragma once

#include "manager/app_settings.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llm_proxy {

class LlmProxyService {
public:
    explicit LlmProxyService(const settings::LlmProxySettings& initial_settings);
    ~LlmProxyService();

    bool Start();
    void Stop();

    uint16_t port() const { return port_.load(); }

    void UpdateSettings(const settings::LlmProxySettings& settings);

private:
    void ServerThread();
    void HandleClient(uintptr_t client_sock);
    bool ParseHttpRequest(uintptr_t sock, std::string& method, std::string& path,
                          std::string& body, bool& keep_alive);
    void HandleChatCompletions(uintptr_t sock, const std::string& body, bool keep_alive);
    void HandleModels(uintptr_t sock, bool keep_alive);
    void SendErrorResponse(uintptr_t sock, int status, const char* status_text,
                           const std::string& message, bool keep_alive);
    void SendResponse(uintptr_t sock, int status, const char* status_text,
                      const std::string& content_type, const std::string& body,
                      bool keep_alive);

    // Describes how an SSE stream ended. Populated by ForwardToUpstream and
    // consumed by WriteLogEntry so we can explain why a stream stopped.
    struct StreamEndInfo {
        // One of: "upstream_done", "upstream_error", "client_disconnect",
        // "proxy_shutdown", "handshake_error", "n/a" (non-streaming).
        std::string reason = "n/a";
        // Human-readable description, may include API error codes.
        std::string detail;
        // WinHTTP error code from GetLastError() on upstream read failure, 0 otherwise.
        uint32_t winhttp_error = 0;
        // WSA error code from WSAGetLastError() on client send failure, 0 otherwise.
        uint32_t wsa_error = 0;
        uint64_t duration_ms = 0;
        uint64_t upstream_bytes = 0;
        uint64_t chunk_count = 0;
        // True if we ever saw an SSE "data: [DONE]" marker from upstream.
        bool saw_done_marker = false;
    };

    bool ForwardToUpstream(uintptr_t client_sock, const settings::LlmModelMapping& mapping,
                           const std::string& modified_body, bool is_streaming, bool keep_alive,
                           int& out_status, std::string& out_response_body,
                           StreamEndInfo& out_stream_end);

    const settings::LlmModelMapping* FindMapping(const std::string& model_name) const;

    void WriteLogEntry(const std::string& request_body, const std::string& response_body,
                       const std::string& alias, const std::string& model,
                       bool is_streaming, int status_code,
                       const StreamEndInfo& stream_end);

    std::string GetLogDir() const;
    void OpenLogFile();
    void CloseLogFile();

    mutable std::mutex settings_mutex_;
    settings::LlmProxySettings settings_;
    std::mutex log_mutex_;
    FILE* log_file_ = nullptr;
    std::string log_dir_;
    std::string current_log_date_;

    std::atomic<uint16_t> port_{0};
    std::atomic<bool> running_{false};
    uintptr_t listen_sock_ = ~(uintptr_t)0;
    std::thread server_thread_;
};

}  // namespace llm_proxy
