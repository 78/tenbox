#pragma once

#include <cstdint>

namespace kvm {

// Returns true if /dev/kvm is usable and the API version matches.
bool IsHypervisorPresent();

// Open /dev/kvm once and cache the fd; returned fd is owned by this module and
// must NOT be closed by the caller.
int GetKvmFd();

// Register/unregister a KVM_IOEVENTFD with DATAMATCH on an MMIO address.
// Identical ioctl path on x86_64 and aarch64 KVM. Returns true on success.
bool RegisterMmioIoEventFd(int vm_fd, uint64_t mmio_addr, uint32_t len,
                           int event_fd, uint32_t datamatch);
bool UnregisterMmioIoEventFd(int vm_fd, uint64_t mmio_addr, uint32_t len,
                             int event_fd, uint32_t datamatch);

} // namespace kvm
