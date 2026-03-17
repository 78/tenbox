#pragma once

#include "common/vm_model.h"
#include "manager/image_source.h"

#include <cstdint>
#include <string>
#include <vector>

namespace settings {

std::string GetDataDir();
std::string DefaultVmStorageDir();
std::string DefaultImageCacheDir(const std::string& data_dir);
std::string GenerateUuid();

struct WindowGeometry {
    int x = -1, y = -1;
    int width = 1024, height = 680;
};

enum class LlmApiType : uint8_t {
    kOpenAiCompletions = 0,  // POST /v1/chat/completions
    // Future:
    // kOpenAiResponses = 1, // POST /v1/responses
    // kAnthropicMessages = 2, // POST /v1/messages
};

struct LlmModelMapping {
    std::string alias;       // "auto", "default", etc.
    std::string target_url;  // "https://api.tenclass.net/v1"
    std::string api_key;     // "sk-..."
    std::string model;       // "qwen-plus"
    LlmApiType api_type = LlmApiType::kOpenAiCompletions;
};

struct LlmProxySettings {
    std::vector<LlmModelMapping> mappings;
};

struct AppSettings {
    WindowGeometry window;
    std::vector<std::string> vm_paths;
    bool show_toolbar = true;
    std::string vm_storage_dir;     // empty = DefaultVmStorageDir()
    std::string image_cache_dir;    // empty = DefaultImageCacheDir(data_dir)
    std::vector<image_source::ImageSource> sources; // empty = use DefaultSources()
    std::string last_selected_source; // name of last selected source
    LlmProxySettings llm_proxy;
};

// Resolve effective directories (returns custom if set, otherwise default).
std::string EffectiveVmStorageDir(const AppSettings& s);
std::string EffectiveImageCacheDir(const AppSettings& s, const std::string& data_dir);

// Returns user-configured sources if non-empty, otherwise DefaultSources().
std::vector<image_source::ImageSource> EffectiveSources(const AppSettings& s);

AppSettings LoadSettings(const std::string& data_dir);
void SaveSettings(const std::string& data_dir, const AppSettings& s);

// Per-VM manifest (vm.json inside the VM directory).
// Paths stored relative; resolved to absolute using vm_dir on load.
bool LoadVmManifest(const std::string& vm_dir, VmSpec& spec);
void SaveVmManifest(const VmSpec& spec);

}  // namespace settings
