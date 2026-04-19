#pragma once

namespace kvm {

// Returns true if /dev/kvm is usable and the API version matches.
bool IsHypervisorPresent();

// Open /dev/kvm once and cache the fd; returned fd is owned by this module and
// must NOT be closed by the caller.
int GetKvmFd();

} // namespace kvm
