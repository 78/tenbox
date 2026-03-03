#pragma once

#include "common/vm_model.h"

#include <string>
#include <vector>

namespace settings {

std::string GetDataDir();
std::string DefaultVmStorageDir();
std::string GenerateUuid();

struct WindowGeometry {
    int x = -1, y = -1;
    int width = 1024, height = 680;
};

struct AppSettings {
    WindowGeometry window;
    std::vector<std::string> vm_paths;
    bool show_toolbar = true;
    bool adaptive_display = true;
};

AppSettings LoadSettings(const std::string& data_dir);
void SaveSettings(const std::string& data_dir, const AppSettings& s);

// Per-VM manifest (vm.json inside the VM directory).
// Paths stored relative; resolved to absolute using vm_dir on load.
bool LoadVmManifest(const std::string& vm_dir, VmSpec& spec);
void SaveVmManifest(const VmSpec& spec);

}  // namespace settings
