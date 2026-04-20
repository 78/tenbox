#include "platform/linux/hypervisor/kvm_platform.h"
#include "core/vmm/types.h"

#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <fcntl.h>
#include <linux/kvm.h>
#include <mutex>
#include <sys/ioctl.h>
#include <unistd.h>

namespace kvm {

namespace {
std::mutex& KvmFdMutex() {
    static std::mutex m;
    return m;
}
int g_kvm_fd = -1;
}

int GetKvmFd() {
    std::lock_guard<std::mutex> lock(KvmFdMutex());
    if (g_kvm_fd >= 0) return g_kvm_fd;

    int fd = ::open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR("kvm: open(/dev/kvm) failed: %s", strerror(errno));
        return -1;
    }

    int api = ::ioctl(fd, KVM_GET_API_VERSION, 0);
    if (api != KVM_API_VERSION) {
        LOG_ERROR("kvm: KVM_GET_API_VERSION=%d, expected %d", api, KVM_API_VERSION);
        ::close(fd);
        return -1;
    }

    g_kvm_fd = fd;
    return g_kvm_fd;
}

bool IsHypervisorPresent() {
    if (::access("/dev/kvm", R_OK | W_OK) != 0) {
        return false;
    }
    return GetKvmFd() >= 0;
}

namespace {
bool DoIoEventFdOp(int vm_fd, uint64_t mmio_addr, uint32_t len, int event_fd,
                   uint32_t datamatch, bool deassign) {
    if (vm_fd < 0 || event_fd < 0) return false;
    // virtio-mmio QueueNotify is 4 bytes wide; KVM accepts 1/2/4/8.
    if (len != 1 && len != 2 && len != 4 && len != 8) return false;

    struct kvm_ioeventfd io{};
    io.datamatch = datamatch;
    io.addr = mmio_addr;
    io.len = len;
    io.fd = event_fd;
    io.flags = KVM_IOEVENTFD_FLAG_DATAMATCH;
    if (deassign) io.flags |= KVM_IOEVENTFD_FLAG_DEASSIGN;

    if (::ioctl(vm_fd, KVM_IOEVENTFD, &io) < 0) {
        LOG_WARN("kvm: KVM_IOEVENTFD(%s addr=0x%" PRIX64
                 " len=%u fd=%d datamatch=0x%X) failed: %s",
                 deassign ? "DEASSIGN" : "ASSIGN",
                 mmio_addr, len, event_fd, datamatch, strerror(errno));
        return false;
    }
    return true;
}
}  // namespace

bool RegisterMmioIoEventFd(int vm_fd, uint64_t mmio_addr, uint32_t len,
                           int event_fd, uint32_t datamatch) {
    return DoIoEventFdOp(vm_fd, mmio_addr, len, event_fd, datamatch, false);
}

bool UnregisterMmioIoEventFd(int vm_fd, uint64_t mmio_addr, uint32_t len,
                             int event_fd, uint32_t datamatch) {
    return DoIoEventFdOp(vm_fd, mmio_addr, len, event_fd, datamatch, true);
}

} // namespace kvm
