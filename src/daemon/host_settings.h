#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace tenbox::daemon {

// Single upstream LLM endpoint that the host-level reverse proxy will route
// requests to. The `alias` is what callers send as the `model` field; the
// proxy rewrites the body to use `model` and adds an `Authorization` header
// derived from `api_key` before forwarding to `target_url`.
struct LlmModelMapping {
    std::string alias;       // "auto", "gpt-4o", etc.
    std::string target_url;  // "https://api.openai.com/v1"
    std::string api_key;     // "sk-..."
    std::string model;       // upstream model name
    std::string api_type = "openai_completions";
};

struct LlmProxySettings {
    std::vector<LlmModelMapping> mappings;
    bool enable_logging = false;
    // 0 means "pick a free port". The actual bound port is reported back via
    // host.resources / the proxy status RPC so the operator can configure
    // their LLM client.
    uint16_t listen_port = 0;
};

struct HostSettings {
    LlmProxySettings llm_proxy;
};

// Load settings from `<data_dir>/host_settings.json`. Missing file or any
// parse error returns defaults (empty proxy mappings, logging disabled).
HostSettings LoadHostSettings(const std::string& data_dir);

// Persist atomically. Returns false on filesystem error; on success the file
// is written to a temp file and renamed so a crash never leaves a half-written
// JSON document on disk.
bool SaveHostSettings(const std::string& data_dir, const HostSettings& settings);

nlohmann::json ToJson(const LlmProxySettings& settings);
nlohmann::json ToJson(const LlmModelMapping& mapping);
LlmProxySettings LlmProxyFromJson(const nlohmann::json& value);

}  // namespace tenbox::daemon
