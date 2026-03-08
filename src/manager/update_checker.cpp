#include "manager/update_checker.h"
#include "manager/http_download.h"
#include "manager/image_source.h"

#include <nlohmann/json.hpp>

namespace update {

static constexpr const char* kVersionUrl = "https://tenbox.ai/api/version.json";

static constexpr const char* kPlatformKey = "windows";

UpdateInfo CheckForUpdate(const std::string& current_version) {
    UpdateInfo info;

    auto result = http::FetchString(kVersionUrl);
    if (!result.success) {
        info.error = result.error;
        return info;
    }

    try {
        auto j = nlohmann::json::parse(result.data);

        if (j.contains("platforms") && j["platforms"].contains(kPlatformKey)) {
            auto& p = j["platforms"][kPlatformKey];
            info.latest_version = p.value("latest_version", "");
            info.release_notes = p.value("release_notes", "");
            info.download_url = p.value("download_url", "");
            info.sha256 = p.value("sha256", "");
            info.size = p.value("size", int64_t{0});
        } else {
            info.latest_version = j.value("latest_version", "");
            info.release_notes = j.value("release_notes", "");
            info.download_url = j.value("download_url", "");
            info.sha256 = j.value("sha256", "");
        }

        if (!info.latest_version.empty() &&
            image_source::CompareVersions(info.latest_version, current_version) > 0) {
            info.update_available = true;
        }
    } catch (...) {
        info.error = "Failed to parse version info";
    }

    return info;
}

}  // namespace update
