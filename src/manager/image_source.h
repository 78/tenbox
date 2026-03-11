#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace image_source {

struct ImageSource {
    std::string name;
    std::string url;
};

struct ImageFile {
    std::string name;
    std::string url;
    std::string sha256;  // empty = skip verification
    uint64_t size = 0;
};

struct ImageEntry {
    std::string id;
    std::string version;
    std::string display_name;
    std::string description;
    std::string min_app_version;
    std::string os;       // "linux", "windows", "macos"
    std::string arch;     // "microvm", "i440fx", "q35"
    std::string platform; // "arm64", "x86_64" (CPU architecture)
    std::vector<ImageFile> files;

    std::string CacheId() const { return id + "-" + version; }

    uint64_t TotalSize() const {
        uint64_t total = 0;
        for (const auto& f : files) total += f.size;
        return total;
    }
};

// Built-in default sources (used when no user override is configured).
std::vector<ImageSource> DefaultSources();

// Parse JSON strings into structs
std::vector<ImageSource> ParseSources(const std::string& json);
std::vector<ImageEntry> ParseImages(const std::string& json);

// Filter: remove entries where arch != "microvm", platform != current CPU, or min_app_version > current
std::vector<ImageEntry> FilterImages(const std::vector<ImageEntry>& images,
                                     const std::string& current_app_version);

// Check if image is fully cached locally (all files present, no .tmp files).
// images_dir is the base images directory (e.g. "{data_dir}/images" or a custom path).
bool IsImageCached(const std::string& images_dir, const ImageEntry& entry);

// Get cache directory path for an image: {images_dir}/{id}-{version}/
std::string ImageCacheDir(const std::string& images_dir, const ImageEntry& entry);

// Get list of locally cached images by scanning images_dir.
std::vector<ImageEntry> GetCachedImages(const std::string& images_dir);

// Save image metadata to cache dir as image.json
void SaveImageMeta(const std::string& cache_dir, const ImageEntry& entry);

// Load image metadata from cache dir
bool LoadImageMeta(const std::string& cache_dir, ImageEntry& entry);

// Compare version strings (returns -1 if a < b, 0 if a == b, 1 if a > b)
int CompareVersions(const std::string& a, const std::string& b);

}  // namespace image_source
