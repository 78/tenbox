#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace http {

// Progress callback: (bytes_downloaded, total_bytes)
// total_bytes may be 0 if Content-Length is unknown
using DownloadProgressCallback = std::function<void(uint64_t downloaded, uint64_t total)>;

struct DownloadResult {
    bool success = false;
    std::string error;
    std::string data;  // for FetchString only
};

// Fetch a URL as a string (for small files like JSON)
DownloadResult FetchString(const std::string& url);

// Download a URL to a file with progress reporting
// Uses .tmp suffix during download, renames to final path on success
// If sha256 is provided (non-empty), verifies hash after download
// cancel_flag can be set to true to abort the download
DownloadResult DownloadFile(const std::string& url,
                            const std::string& dest_path,
                            const std::string& sha256 = "",
                            DownloadProgressCallback progress = nullptr,
                            std::atomic<bool>* cancel_flag = nullptr);

// Calculate SHA256 hash of a file (returns lowercase hex string)
std::string FileSha256(const std::string& path);

}  // namespace http
