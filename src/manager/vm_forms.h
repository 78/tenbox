#pragma once

#include "common/vm_model.h"
#include "manager/manager_service.h"

#include <string>
#include <vector>

struct VmEditForm {
    std::string vm_id;
    std::string name;
    bool debug_mode = false;
    std::vector<PortForward> port_forwards;
    uint64_t memory_mb = 4096;
    uint32_t cpu_count = 4;
    bool apply_on_next_boot = false;
};

struct ValidationResult {
    bool ok = false;
    std::string message;
};

ValidationResult ValidateCreateRequest(const VmCreateRequest& req);
ValidationResult ValidateEditForm(const VmEditForm& form, VmPowerState current_state);

VmMutablePatch BuildVmPatch(const VmEditForm& form, const VmSpec& current_spec);
