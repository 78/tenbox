#include "daemon/host_settings.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace tenbox::daemon {
namespace fs = std::filesystem;

namespace {
constexpr const char* kHostSettingsFile = "host_settings.json";

bool WriteFileAtomic(const fs::path& target, const std::string& contents) {
    fs::path tmp = target;
    tmp += ".tmp";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!out) return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}
}  // namespace

nlohmann::json ToJson(const LlmModelMapping& mapping) {
    return {
        {"alias", mapping.alias},
        {"target_url", mapping.target_url},
        {"api_key", mapping.api_key},
        {"model", mapping.model},
        {"api_type", mapping.api_type},
    };
}

nlohmann::json ToJson(const LlmProxySettings& settings) {
    nlohmann::json mappings = nlohmann::json::array();
    for (const auto& m : settings.mappings) mappings.push_back(ToJson(m));
    return {
        {"mappings", std::move(mappings)},
        {"enable_logging", settings.enable_logging},
        {"listen_port", settings.listen_port},
    };
}

LlmProxySettings LlmProxyFromJson(const nlohmann::json& value) {
    LlmProxySettings out;
    if (!value.is_object()) return out;
    out.enable_logging = value.value("enable_logging", false);
    out.listen_port = static_cast<uint16_t>(value.value("listen_port", 0));
    if (value.contains("mappings") && value["mappings"].is_array()) {
        for (const auto& entry : value["mappings"]) {
            if (!entry.is_object()) continue;
            LlmModelMapping m;
            m.alias = entry.value("alias", "");
            m.target_url = entry.value("target_url", "");
            m.api_key = entry.value("api_key", "");
            m.model = entry.value("model", "");
            m.api_type = entry.value("api_type", "openai_completions");
            // Skip rows missing the minimum required fields. The console UI
            // will refuse to submit them, but a hand-edited JSON could
            // sneak past — drop silently rather than crash later.
            if (m.alias.empty() || m.target_url.empty()) continue;
            out.mappings.push_back(std::move(m));
        }
    }
    return out;
}

HostSettings LoadHostSettings(const std::string& data_dir) {
    HostSettings out;
    fs::path path = fs::path(data_dir) / kHostSettingsFile;
    std::ifstream in(path);
    if (!in) return out;
    auto json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return out;
    if (json.contains("llm_proxy")) {
        out.llm_proxy = LlmProxyFromJson(json["llm_proxy"]);
    }
    return out;
}

bool SaveHostSettings(const std::string& data_dir, const HostSettings& settings) {
    nlohmann::json doc = {
        {"llm_proxy", ToJson(settings.llm_proxy)},
    };
    return WriteFileAtomic(fs::path(data_dir) / kHostSettingsFile, doc.dump(2));
}

}  // namespace tenbox::daemon
