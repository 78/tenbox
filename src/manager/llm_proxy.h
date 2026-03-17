#pragma once

#include "manager/app_settings.h"

#include <atomic>
#include <cstdint>
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

    bool ForwardToUpstream(uintptr_t client_sock, const settings::LlmModelMapping& mapping,
                           const std::string& modified_body, bool is_streaming, bool keep_alive);

    const settings::LlmModelMapping* FindMapping(const std::string& model_name) const;

    mutable std::mutex settings_mutex_;
    settings::LlmProxySettings settings_;

    std::atomic<uint16_t> port_{0};
    std::atomic<bool> running_{false};
    uintptr_t listen_sock_ = ~(uintptr_t)0;
    std::thread server_thread_;
};

}  // namespace llm_proxy
