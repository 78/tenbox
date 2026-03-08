#pragma once

#include <string>

namespace update {

struct UpdateInfo {
    std::string latest_version;
    std::string download_url;
    std::string release_notes;
    std::string sha256;
    int64_t size = 0;
    bool update_available = false;
    std::string error;
};

// Fetch version info from the server and compare with current version.
// This is a blocking call - run it on a background thread.
UpdateInfo CheckForUpdate(const std::string& current_version);

}  // namespace update
