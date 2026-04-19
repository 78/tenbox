#include "platform/linux/hypervisor/kvm_platform.h"
#include "core/vmm/types.h"

#include <cerrno>
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

} // namespace kvm
