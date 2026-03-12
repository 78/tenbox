#include "manager/vm_forms.h"

namespace {
bool SameForwards(const std::vector<PortForward>& a, const std::vector<PortForward>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].host_port != b[i].host_port || a[i].guest_port != b[i].guest_port) {
            return false;
        }
    }
    return true;
}
}  // namespace

ValidationResult ValidateCreateRequest(const VmCreateRequest& req) {
    if (req.name.empty()) return {false, "name is required"};
    if (req.source_kernel.empty()) return {false, "kernel path is required"};
    if (req.memory_mb < 16) return {false, "minimum memory is 16 MB"};
    if (req.cpu_count < 1 || req.cpu_count > 128) {
        return {false, "cpu_count must be in [1, 128]"};
    }
    return {true, ""};
}

ValidationResult ValidateEditForm(const VmEditForm& form, VmPowerState current_state) {
    if (form.vm_id.empty()) return {false, "vm_id is required"};
    if (form.name.empty()) return {false, "name is required"};
    if (form.memory_mb < 16) return {false, "minimum memory is 16 MB"};
    if (form.cpu_count < 1 || form.cpu_count > 128) {
        return {false, "cpu_count must be in [1, 128]"};
    }

    const bool running = current_state == VmPowerState::kRunning ||
                         current_state == VmPowerState::kStarting;
    if (running && !form.apply_on_next_boot) {
        return {true, "nat/port_forwards can apply online; cpu/memory requires power off"};
    }
    return {true, ""};
}

VmMutablePatch BuildVmPatch(const VmEditForm& form, const VmSpec& current_spec) {
    VmMutablePatch patch;
    patch.apply_on_next_boot = form.apply_on_next_boot;
    if (form.name != current_spec.name) {
        patch.name = form.name;
    }
    if (form.debug_mode != current_spec.debug_mode) {
        patch.debug_mode = form.debug_mode;
    }
    if (!SameForwards(form.port_forwards, current_spec.port_forwards)) {
        patch.port_forwards = form.port_forwards;
    }
    if (form.memory_mb != current_spec.memory_mb) {
        patch.memory_mb = form.memory_mb;
    }
    if (form.cpu_count != current_spec.cpu_count) {
        patch.cpu_count = form.cpu_count;
    }
    return patch;
}
