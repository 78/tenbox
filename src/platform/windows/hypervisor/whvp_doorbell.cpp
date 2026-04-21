#include "platform/windows/hypervisor/whvp_doorbell.h"
#include "core/vmm/types.h"

#include <cstring>

namespace whvp {

namespace {

constexpr DWORD kMaxDoorbellSlots =
    MAXIMUM_WAIT_OBJECTS > 0 ? static_cast<DWORD>(MAXIMUM_WAIT_OBJECTS) - 1u : 63u;

// WHvCreateNotificationPort / WHvDeleteNotificationPort (notification-port
// API, 1809+) and WHvRegisterPartitionDoorbellEvent /
// WHvUnregisterPartitionDoorbellEvent (legacy doorbell API, also post-1803)
// are not present on Windows 10 1803. Referencing any of them directly would
// add static imports to WinHvPlatform.dll that cause the loader to reject the
// EXE on 1803 with "entry point not found" before main() runs. Resolve them
// dynamically and call through these function pointers instead.
using PFN_WHvCreateNotificationPort = HRESULT(WINAPI*)(
    WHV_PARTITION_HANDLE, const WHV_NOTIFICATION_PORT_PARAMETERS*, HANDLE,
    WHV_NOTIFICATION_PORT_HANDLE*);
using PFN_WHvDeleteNotificationPort = HRESULT(WINAPI*)(
    WHV_PARTITION_HANDLE, WHV_NOTIFICATION_PORT_HANDLE);
using PFN_WHvRegisterPartitionDoorbellEvent = HRESULT(WINAPI*)(
    WHV_PARTITION_HANDLE, const WHV_DOORBELL_MATCH_DATA*, HANDLE);
using PFN_WHvUnregisterPartitionDoorbellEvent = HRESULT(WINAPI*)(
    WHV_PARTITION_HANDLE, const WHV_DOORBELL_MATCH_DATA*);

static PFN_WHvCreateNotificationPort g_pfnCreateNotificationPort = nullptr;
static PFN_WHvDeleteNotificationPort g_pfnDeleteNotificationPort = nullptr;
static PFN_WHvRegisterPartitionDoorbellEvent g_pfnRegisterDoorbell = nullptr;
static PFN_WHvUnregisterPartitionDoorbellEvent g_pfnUnregisterDoorbell = nullptr;

void FillDoorbellMatch(WHV_DOORBELL_MATCH_DATA* m, uint64_t mmio_addr, uint32_t len,
                       uint32_t datamatch) {
    memset(m, 0, sizeof(*m));
    m->GuestAddress = mmio_addr;
    m->Value = datamatch;
    m->Length = len;
    m->MatchOnValue = 1;
    m->MatchOnLength = 1;
}

// IsWHv*Present() is not exported from all WinHvPlatform.lib builds; resolve
// the real entry points from the loaded WinHvPlatform.dll instead. Also
// caches the notification-port function pointers for later use so we never
// take the address of those symbols at link time (breaks Windows 1803).
static bool WhvDoorbellApisAvailable(bool* use_notification_port) {
    HMODULE whv = GetModuleHandleW(L"WinHvPlatform.dll");
    if (!whv) {
        // The DLL is normally loaded already because other WHv* calls in
        // whvp_vm.cpp / whvp_vcpu.cpp import it; load explicitly in case we
        // got here first.
        whv = LoadLibraryW(L"WinHvPlatform.dll");
    }
    if (!whv) {
        *use_notification_port = false;
        return false;
    }

    auto pCreate = reinterpret_cast<PFN_WHvCreateNotificationPort>(
        GetProcAddress(whv, "WHvCreateNotificationPort"));
    auto pDelete = reinterpret_cast<PFN_WHvDeleteNotificationPort>(
        GetProcAddress(whv, "WHvDeleteNotificationPort"));
    const bool notify = (pCreate != nullptr && pDelete != nullptr);

    auto pRegDb = reinterpret_cast<PFN_WHvRegisterPartitionDoorbellEvent>(
        GetProcAddress(whv, "WHvRegisterPartitionDoorbellEvent"));
    auto pUnregDb = reinterpret_cast<PFN_WHvUnregisterPartitionDoorbellEvent>(
        GetProcAddress(whv, "WHvUnregisterPartitionDoorbellEvent"));
    const bool legacy = (pRegDb != nullptr && pUnregDb != nullptr);

    if (notify) {
        g_pfnCreateNotificationPort = pCreate;
        g_pfnDeleteNotificationPort = pDelete;
        *use_notification_port = true;
        return true;
    }
    if (legacy) {
        g_pfnRegisterDoorbell = pRegDb;
        g_pfnUnregisterDoorbell = pUnregDb;
        *use_notification_port = false;
        return true;
    }
    *use_notification_port = false;
    return false;
}

} // namespace

WhvpDoorbellRegistrar::WhvpDoorbellRegistrar(WHV_PARTITION_HANDLE partition)
    : partition_(partition) {
    if (!partition_) {
        available_ = false;
        return;
    }

    wakeup_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeup_event_) {
        LOG_WARN("WHPX doorbell: CreateEvent(wakeup) failed: %lu", GetLastError());
        available_ = false;
        return;
    }

    available_ = WhvDoorbellApisAvailable(&use_notification_port_api_);
    if (!available_)
        LOG_INFO("WHPX doorbell: APIs not present on this host");

    if (available_) {
        dispatcher_thread_ = std::thread(&WhvpDoorbellRegistrar::DispatcherLoop, this);
    } else if (wakeup_event_) {
        CloseHandle(wakeup_event_);
        wakeup_event_ = nullptr;
    }
}

WhvpDoorbellRegistrar::~WhvpDoorbellRegistrar() { Shutdown(); }

bool WhvpDoorbellRegistrar::Register(uint64_t mmio_addr, uint32_t len, uint32_t datamatch,
                                     std::function<void()> cb) {
    if (!available_ || !cb) return false;
    if (shutdown_done_.load(std::memory_order_acquire)) return false;

    std::lock_guard<std::mutex> lk(mu_);
    if (slots_.size() >= kMaxDoorbellSlots) {
        LOG_WARN("WHPX doorbell: slot limit (%u) reached; MMIO fallback for extra queues",
                 static_cast<unsigned>(kMaxDoorbellSlots));
        return false;
    }

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) {
        LOG_WARN("WHPX doorbell: CreateEvent failed: %lu", GetLastError());
        return false;
    }

    WHV_DOORBELL_MATCH_DATA match{};
    FillDoorbellMatch(&match, mmio_addr, len, datamatch);

    HRESULT hr = E_FAIL;
    ApiKind api = ApiKind::None;
    WHV_NOTIFICATION_PORT_HANDLE port = nullptr;

    if (use_notification_port_api_ && g_pfnCreateNotificationPort) {
        WHV_NOTIFICATION_PORT_PARAMETERS params{};
        memset(&params, 0, sizeof(params));
        params.NotificationPortType = WHvNotificationPortTypeDoorbell;
        params.ConnectionVtl = 0;
        params.Doorbell = match;
        hr = g_pfnCreateNotificationPort(partition_, &params, ev, &port);
        api = ApiKind::NotificationPort;
    } else if (g_pfnRegisterDoorbell) {
        hr = g_pfnRegisterDoorbell(partition_, &match, ev);
        api = ApiKind::LegacyDoorbell;
    } else {
        hr = E_NOTIMPL;
    }

    if (FAILED(hr)) {
        CloseHandle(ev);
        LOG_WARN("WHPX doorbell: register failed (gpa=0x%llX q=%u): 0x%08lX",
                 static_cast<unsigned long long>(mmio_addr),
                 static_cast<unsigned>(datamatch), hr);
        return false;
    }

    auto slot = std::make_unique<Slot>();
    slot->mmio_addr = mmio_addr;
    slot->len = len;
    slot->datamatch = datamatch;
    slot->event = ev;
    slot->cb = std::move(cb);
    slot->api = api;
    slot->port_handle = port;
    slot->legacy_match = match;

    slots_.push_back(std::move(slot));

    // Wake dispatcher so the next Wait includes the new event handle.
    if (!SetEvent(wakeup_event_)) {
        LOG_WARN("WHPX doorbell: SetEvent(wakeup) failed: %lu", GetLastError());
    }

    return true;
}

void WhvpDoorbellRegistrar::Shutdown() {
    if (shutdown_done_.exchange(true, std::memory_order_acq_rel)) return;

    stop_.store(true, std::memory_order_release);
    if (wakeup_event_) SetEvent(wakeup_event_);

    if (dispatcher_thread_.joinable()) dispatcher_thread_.join();

    std::lock_guard<std::mutex> lk(mu_);
    for (auto& up : slots_) {
        if (!up) continue;
        if (up->api == ApiKind::NotificationPort && up->port_handle) {
            if (g_pfnDeleteNotificationPort) {
                HRESULT hr = g_pfnDeleteNotificationPort(partition_, up->port_handle);
                if (FAILED(hr)) {
                    LOG_WARN("WHPX doorbell: WHvDeleteNotificationPort failed: 0x%08lX", hr);
                }
            }
            up->port_handle = nullptr;
        } else if (up->api == ApiKind::LegacyDoorbell) {
            if (g_pfnUnregisterDoorbell) {
                HRESULT hr =
                    g_pfnUnregisterDoorbell(partition_, &up->legacy_match);
                if (FAILED(hr)) {
                    LOG_WARN(
                        "WHPX doorbell: WHvUnregisterPartitionDoorbellEvent failed: 0x%08lX",
                        hr);
                }
            }
        }
        if (up->event) {
            CloseHandle(up->event);
            up->event = nullptr;
        }
    }
    slots_.clear();

    if (wakeup_event_) {
        CloseHandle(wakeup_event_);
        wakeup_event_ = nullptr;
    }
}

void WhvpDoorbellRegistrar::DispatcherLoop() {
    while (!stop_.load(std::memory_order_acquire)) {
        std::vector<HANDLE> handles;
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lk(mu_);
            handles.reserve(slots_.size() + 1);
            handles.push_back(wakeup_event_);
            for (const auto& up : slots_) {
                if (up && up->event) {
                    handles.push_back(up->event);
                    callbacks.push_back(up->cb);
                }
            }
        }

        const DWORD n = static_cast<DWORD>(handles.size());
        if (n == 0) break;

        const DWORD r = WaitForMultipleObjects(n, handles.data(), FALSE, INFINITE);
        if (r == WAIT_FAILED) {
            LOG_WARN("WHPX doorbell: WaitForMultipleObjects failed: %lu", GetLastError());
            Sleep(10);
            continue;
        }

        if (r == WAIT_OBJECT_0) {
            if (stop_.load(std::memory_order_acquire)) break;
            continue;
        }

        if (r >= WAIT_OBJECT_0 + 1 && r < WAIT_OBJECT_0 + n) {
            const size_t idx = static_cast<size_t>(r - WAIT_OBJECT_0 - 1);
            if (idx < callbacks.size() && callbacks[idx]) callbacks[idx]();
        }
    }
}

} // namespace whvp
