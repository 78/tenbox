#include "platform/windows/hypervisor/whvp_dyn.h"
#include "core/vmm/types.h"

#include <atomic>
#include <mutex>

namespace whvp {
namespace dyn {

namespace {

using PFN_WHvRequestInterrupt = HRESULT(WINAPI*)(
    WHV_PARTITION_HANDLE, const WHV_INTERRUPT_CONTROL*, UINT32);
using PFN_WHvCreateNotificationPort = HRESULT(WINAPI*)(
    WHV_PARTITION_HANDLE, const WHV_NOTIFICATION_PORT_PARAMETERS*, HANDLE,
    WHV_NOTIFICATION_PORT_HANDLE*);
using PFN_WHvDeleteNotificationPort = HRESULT(WINAPI*)(
    WHV_PARTITION_HANDLE, WHV_NOTIFICATION_PORT_HANDLE);

struct State {
    std::atomic<bool> loaded{false};
    HMODULE platform = nullptr;
    PFN_WHvRequestInterrupt       request_interrupt = nullptr;
    PFN_WHvCreateNotificationPort create_notification_port = nullptr;
    PFN_WHvDeleteNotificationPort delete_notification_port = nullptr;
};

static std::mutex g_mu;
static State      g_st;

} // namespace

bool Load() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_st.loaded.load(std::memory_order_acquire)) return true;

    HMODULE p = GetModuleHandleW(L"WinHvPlatform.dll");
    if (!p) {
        p = LoadLibraryW(L"WinHvPlatform.dll");
    }
    if (!p) {
        LOG_ERROR("WHPX dyn: LoadLibrary(WinHvPlatform.dll) failed: %lu",
                  GetLastError());
        return false;
    }

    g_st.platform = p;
    g_st.request_interrupt = reinterpret_cast<PFN_WHvRequestInterrupt>(
        GetProcAddress(p, "WHvRequestInterrupt"));
    g_st.create_notification_port = reinterpret_cast<PFN_WHvCreateNotificationPort>(
        GetProcAddress(p, "WHvCreateNotificationPort"));
    g_st.delete_notification_port = reinterpret_cast<PFN_WHvDeleteNotificationPort>(
        GetProcAddress(p, "WHvDeleteNotificationPort"));

    g_st.loaded.store(true, std::memory_order_release);

    LOG_INFO("WHPX dyn: RequestInterrupt=%s, NotificationPort=%s",
             g_st.request_interrupt ? "yes" : "no (likely 1803)",
             (g_st.create_notification_port && g_st.delete_notification_port)
                 ? "yes" : "no");
    return true;
}

void Unload() {
    std::lock_guard<std::mutex> lk(g_mu);
    // We obtained the module via GetModuleHandle/LoadLibrary; a single
    // FreeLibrary balances LoadLibrary. GetModuleHandle doesn't add a
    // ref, so the best we can do safely is leave it loaded for process
    // lifetime. No-op here.
    g_st.platform = nullptr;
    g_st.request_interrupt = nullptr;
    g_st.create_notification_port = nullptr;
    g_st.delete_notification_port = nullptr;
    g_st.loaded.store(false, std::memory_order_release);
}

bool HasRequestInterrupt() {
    return g_st.loaded.load(std::memory_order_acquire)
        && g_st.request_interrupt != nullptr;
}
bool HasCreateNotificationPort() {
    return g_st.loaded.load(std::memory_order_acquire)
        && g_st.create_notification_port != nullptr;
}
bool HasDeleteNotificationPort() {
    return g_st.loaded.load(std::memory_order_acquire)
        && g_st.delete_notification_port != nullptr;
}

HRESULT RequestInterrupt(WHV_PARTITION_HANDLE partition,
                         const WHV_INTERRUPT_CONTROL* control,
                         UINT32 control_size) {
    auto fn = g_st.request_interrupt;
    if (!fn) return E_NOTIMPL;
    return fn(partition, control, control_size);
}
HRESULT CreateNotificationPort(
    WHV_PARTITION_HANDLE partition,
    const WHV_NOTIFICATION_PORT_PARAMETERS* parameters,
    HANDLE event, WHV_NOTIFICATION_PORT_HANDLE* port) {
    auto fn = g_st.create_notification_port;
    if (!fn) return E_NOTIMPL;
    return fn(partition, parameters, event, port);
}
HRESULT DeleteNotificationPort(WHV_PARTITION_HANDLE partition,
                               WHV_NOTIFICATION_PORT_HANDLE port) {
    auto fn = g_st.delete_notification_port;
    if (!fn) return E_NOTIMPL;
    return fn(partition, port);
}

} // namespace dyn
} // namespace whvp
