#pragma once

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
};

struct ImageEntry {
    std::string id;
    std::string version;
    std::string display_name;
    std::string description;
    std::string min_app_version;
    std::string os;    // "linux", "windows", "macos"
    std::string arch;  // "microvm", "i440fx", "q35"
    std::vector<ImageFile> files;

    std::string CacheId() const { return id + "-" + version; }
};

// Parse JSON strings into structs
std::vector<ImageSource> ParseSources(const std::string& json);
std::vector<ImageEntry> ParseImages(const std::string& json);

// Filter: remove entries where arch != "microvm" or min_app_version > current
std::vector<ImageEntry> FilterImages(const std::vector<ImageEntry>& images,
                                     const std::string& current_app_version);

// Check if image is fully cached locally (all files present, no .tmp files)
bool IsImageCached(const std::string& data_dir, const ImageEntry& entry);

// Get cache directory path for an image: {data_dir}/images/{id}-{version}/
std::string ImageCacheDir(const std::string& data_dir, const ImageEntry& entry);

// Get list of locally cached images by scanning {data_dir}/images/
std::vector<ImageEntry> GetCachedImages(const std::string& data_dir);

// Save image metadata to cache dir as image.json
void SaveImageMeta(const std::string& cache_dir, const ImageEntry& entry);

// Load image metadata from cache dir
bool LoadImageMeta(const std::string& cache_dir, ImageEntry& entry);

// Compare version strings (returns -1 if a < b, 0 if a == b, 1 if a > b)
int CompareVersions(const std::string& a, const std::string& b);

}  // namespace image_source
