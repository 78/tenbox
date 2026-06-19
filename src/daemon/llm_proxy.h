#pragma once

// LLM API reverse proxy that runs inside agentsphered. Listens on 127.0.0.1
// (reachable from guests as 10.0.2.2 through QEMU's slirp NAT), receives
// OpenAI-compatible HTTP requests, looks up the requested model alias in
// the host-level LlmProxySettings, rewrites the request body to use the
// upstream model name, and forwards to the configured upstream endpoint
// using libcurl. Streaming responses (text/event-stream) are forwarded
// chunk-by-chunk so the guest sees tokens with the same latency as
// talking to the upstream provider directly.
//
// Mirrors the design of src/manager/llm_proxy.{h,cpp} (the Win32
// WinHTTP-based implementation) so the JSON-on-disk log format and the
// console UI consuming it stay compatible.

#include "daemon/host_settings.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace tenbox::daemon {

class LlmProxyService {
public:
    LlmProxyService(const LlmProxySettings& initial_settings, std::string data_dir);
    ~LlmProxyService();

    LlmProxyService(const LlmProxyService&) = delete;
    LlmProxyService& operator=(const LlmProxyService&) = delete;

    // Bind the listen socket and spin up the accept thread. Returns false
    // on bind/listen failure; the caller can retry after fixing settings.
    // Idempotent: a second call while already running is a no-op returning
    // true.
    bool Start();
    void Stop();

    // Actual bound TCP port (after Start). Reflects the kernel-chosen port
    // when LlmProxySettings::listen_port is 0. Returns 0 when stopped.
    uint16_t port() const { return port_.load(); }

    // Replace the live mapping table and toggle logging. Safe to call
    // while requests are in flight; in-flight requests use the snapshot
    // they took at FindMapping time.
    //
    // Note: changes to listen_port do NOT relocate the live socket. The
    // caller (CloudTunnel) is expected to compare the desired port to the
    // running port and Stop()/Start() if a relocation is wanted.
    void UpdateSettings(const LlmProxySettings& settings);

private:
    // See src/manager/llm_proxy.h for the rationale; same fields appear
    // in the daemon jsonl log.
    struct StreamEndInfo {
        std::string reason = "n/a";
        std::string detail;
        // libcurl CURLcode when upstream errored, 0 otherwise.
        int curl_code = 0;
        // errno from the client send() that broke the stream, 0 otherwise.
        int client_errno = 0;
        uint64_t duration_ms = 0;
        uint64_t upstream_bytes = 0;
        uint64_t chunk_count = 0;
        bool saw_done_marker = false;
    };

    void ServerThread();
    void HandleClient(int client_fd);

    bool ParseHttpRequest(int client_fd, std::string& method, std::string& path,
                          std::string& body, bool& keep_alive);

    void HandleChatCompletions(int client_fd, const std::string& body, bool keep_alive);
    void HandleModels(int client_fd, bool keep_alive);

    bool ForwardToUpstream(int client_fd, const LlmModelMapping& mapping,
                           const std::string& modified_body, bool is_streaming, bool& keep_alive,
                           int& out_status, std::string& out_response_body,
                           StreamEndInfo& out_stream_end);

    void SendErrorResponse(int client_fd, int status, const char* status_text,
                           const std::string& message, bool keep_alive);
    void SendResponse(int client_fd, int status, const char* status_text,
                      const std::string& content_type, const std::string& body,
                      bool keep_alive);

    LlmModelMapping FindMappingCopy(const std::string& alias, bool& found) const;

    void WriteLogEntry(const std::string& request_body, const std::string& response_body,
                       const std::string& alias, const std::string& model,
                       bool is_streaming, int status_code,
                       const StreamEndInfo& stream_end);

    std::string LogDir() const;
    void OpenLogFileLocked();   // Caller holds log_mutex_.
    void CloseLogFileLocked();  // Caller holds log_mutex_.
    void RotateLogFileIfNeededLocked();

    const std::string data_dir_;

    mutable std::mutex settings_mutex_;
    LlmProxySettings settings_;

    mutable std::mutex log_mutex_;
    FILE* log_file_ = nullptr;
    std::string current_log_date_;

    std::atomic<uint16_t> port_{0};
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    std::thread server_thread_;
};

}  // namespace tenbox::daemon
