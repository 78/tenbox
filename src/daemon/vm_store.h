#pragma once

#include "daemon/daemon_types.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tenbox::daemon {

class VmStore {
public:
    explicit VmStore(std::string data_dir);

    bool Load(std::string* error);
    bool SaveVm(const VmSpec& spec, std::string* error) const;

    std::vector<VmRecord> List() const;
    std::optional<VmRecord> Get(const std::string& vm_id) const;

    bool Create(const VmSpec& spec, VmRecord* created, std::string* error);
    bool UpdateSpec(const std::string& vm_id, const VmSpec& spec, std::string* error);
    bool Remove(const std::string& vm_id, std::string* error);
    bool UpdateRuntime(const std::string& vm_id, const VmRuntimeInfo& runtime);
    bool SetFailure(const std::string& vm_id, FailureInfo failure);

    std::filesystem::path VmRoot() const;
    const std::string& data_dir() const { return data_dir_; }

private:
    bool LoadVmDir(const std::filesystem::path& vm_dir, std::string* error);

    std::string data_dir_;
    mutable std::mutex mutex_;
    std::vector<VmRecord> vms_;
};

}  // namespace tenbox::daemon
