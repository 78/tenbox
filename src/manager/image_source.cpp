#include "manager/image_source.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace image_source {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::vector<ImageSource> ParseSources(const std::string& json_str) {
    std::vector<ImageSource> result;
    try {
        auto j = json::parse(json_str);
        if (!j.contains("sources") || !j["sources"].is_array()) {
            return result;
        }
        for (const auto& src : j["sources"]) {
            ImageSource s;
            s.name = src.value("name", "");
            s.url = src.value("url", "");
            if (!s.url.empty()) {
                result.push_back(std::move(s));
            }
        }
    } catch (...) {
    }
    return result;
}

std::vector<ImageEntry> ParseImages(const std::string& json_str) {
    std::vector<ImageEntry> result;
    try {
        auto j = json::parse(json_str);
        if (!j.contains("images") || !j["images"].is_array()) {
            return result;
        }
        for (const auto& img : j["images"]) {
            ImageEntry e;
            e.id = img.value("id", "");
            e.version = img.value("version", "");
            e.display_name = img.value("name", "");
            e.description = img.value("description", "");
            e.min_app_version = img.value("min_app_version", "0.0.0");
            e.os = img.value("os", "linux");
            e.arch = img.value("arch", "microvm");

            if (img.contains("files") && img["files"].is_array()) {
                for (const auto& f : img["files"]) {
                    ImageFile file;
                    file.name = f.value("name", "");
                    file.url = f.value("url", "");
                    file.sha256 = f.value("sha256", "");
                    file.size = f.value("size", uint64_t(0));
                    if (!file.name.empty() && !file.url.empty()) {
                        e.files.push_back(std::move(file));
                    }
                }
            }

            if (!e.id.empty() && !e.version.empty() && !e.files.empty()) {
                result.push_back(std::move(e));
            }
        }
    } catch (...) {
    }
    return result;
}

int CompareVersions(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& v) -> std::vector<int> {
        std::vector<int> parts;
        std::istringstream ss(v);
        std::string part;
        while (std::getline(ss, part, '.')) {
            try {
                parts.push_back(std::stoi(part));
            } catch (...) {
                parts.push_back(0);
            }
        }
        while (parts.size() < 3) parts.push_back(0);
        return parts;
    };

    auto va = parse(a);
    auto vb = parse(b);

    for (size_t i = 0; i < 3; ++i) {
        if (va[i] < vb[i]) return -1;
        if (va[i] > vb[i]) return 1;
    }
    return 0;
}

std::vector<ImageEntry> FilterImages(const std::vector<ImageEntry>& images,
                                     const std::string& current_app_version) {
    std::vector<ImageEntry> result;
    for (const auto& img : images) {
        if (img.arch != "microvm") {
            continue;
        }
        if (CompareVersions(img.min_app_version, current_app_version) > 0) {
            continue;
        }
        result.push_back(img);
    }
    return result;
}

std::string ImageCacheDir(const std::string& images_dir, const ImageEntry& entry) {
    return (fs::path(images_dir) / entry.CacheId()).string();
}

bool IsImageCached(const std::string& images_dir, const ImageEntry& entry) {
    std::string cache_dir = ImageCacheDir(images_dir, entry);
    if (!fs::exists(cache_dir) || !fs::is_directory(cache_dir)) {
        return false;
    }

    for (const auto& file : entry.files) {
        fs::path file_path = fs::path(cache_dir) / file.name;
        if (!fs::exists(file_path)) {
            return false;
        }
        fs::path tmp_path = fs::path(cache_dir) / (file.name + ".tmp");
        if (fs::exists(tmp_path)) {
            return false;
        }
    }
    return true;
}

std::vector<ImageEntry> GetCachedImages(const std::string& images_dir) {
    std::vector<ImageEntry> result;

    if (!fs::exists(images_dir) || !fs::is_directory(images_dir)) {
        return result;
    }

    for (const auto& entry : fs::directory_iterator(images_dir)) {
        if (!entry.is_directory()) continue;

        ImageEntry img;
        if (LoadImageMeta(entry.path().string(), img)) {
            if (IsImageCached(images_dir, img)) {
                result.push_back(std::move(img));
            }
        }
    }
    return result;
}

void SaveImageMeta(const std::string& cache_dir, const ImageEntry& entry) {
    std::error_code ec;
    fs::create_directories(cache_dir, ec);

    json j;
    j["id"] = entry.id;
    j["version"] = entry.version;
    j["name"] = entry.display_name;
    j["description"] = entry.description;
    j["min_app_version"] = entry.min_app_version;
    j["os"] = entry.os;
    j["arch"] = entry.arch;

    json files = json::array();
    for (const auto& f : entry.files) {
        json file_j;
        file_j["name"] = f.name;
        file_j["url"] = f.url;
        file_j["sha256"] = f.sha256;
        file_j["size"] = f.size;
        files.push_back(file_j);
    }
    j["files"] = files;

    fs::path meta_path = fs::path(cache_dir) / "image.json";
    std::ofstream ofs(meta_path);
    if (ofs) {
        ofs << j.dump(2);
    }
}

bool LoadImageMeta(const std::string& cache_dir, ImageEntry& entry) {
    fs::path meta_path = fs::path(cache_dir) / "image.json";
    if (!fs::exists(meta_path)) {
        return false;
    }

    std::ifstream ifs(meta_path);
    if (!ifs) {
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());

    auto images = ParseImages("{\"images\":[" + content + "]}");
    if (images.empty()) {
        return false;
    }

    entry = std::move(images[0]);
    return true;
}

}  // namespace image_source
