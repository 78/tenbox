#include "platform/windows/hypervisor/whvp_doorbell.h"
#include "platform/windows/hypervisor/whvp_dyn.h"
#include "core/vmm/types.h"

#include <cstring>

namespace whvp {

namespace {

constexpr DWORD kMaxDoorbellSlots =
    MAXIMUM_WAIT_OBJECTS > 0 ? static_cast<DWORD>(MAXIMUM_WAIT_OBJECTS) - 1u : 63u;

void FillDoorbellMatch(WHV_DOORBELL_MATCH_DATA* m, uint64_t mmio_addr, uint32_t len,
                       uint32_t datamatch) {
    memset(m, 0, sizeof(*m));
    m->GuestAddress = mmio_addr;
    m->Value = datamatch;
    m->Length = len;
    m->MatchOnValue = 1;
    m->MatchOnLength = 1;
}

// Notification port APIs (1809+) are routed through the whvp::dyn loader.
// WHvRegisterPartitionDoorbellEvent is intentionally ignored: it has shipped
// broken on several pre-19H1 builds and can crash the caller inside
// vmcompute.dll when doorbells are registered across different GPAs.
static bool WhvDoorbellApisAvailable() {
    return dyn::HasCreateNotificationPort() && dyn::HasDeleteNotificationPort();
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

    available_ = WhvDoorbellApisAvailable();
    if (available_) {
        dispatcher_thread_ = std::thread(&WhvpDoorbellRegistrar::DispatcherLoop, this);
    } else {
        LOG_INFO("WHPX doorbell: notification-port API not present on this host");
        if (wakeup_event_) {
            CloseHandle(wakeup_event_);
            wakeup_event_ = nullptr;
        }
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

    WHV_NOTIFICATION_PORT_PARAMETERS params{};
    memset(&params, 0, sizeof(params));
    params.NotificationPortType = WHvNotificationPortTypeDoorbell;
    params.ConnectionVtl = 0;
    params.Doorbell = match;

    WHV_NOTIFICATION_PORT_HANDLE port = nullptr;
    HRESULT hr = dyn::CreateNotificationPort(partition_, &params, ev, &port);
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
    slot->port_handle = port;

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
        if (up->port_handle && dyn::HasDeleteNotificationPort()) {
            HRESULT hr = dyn::DeleteNotificationPort(partition_, up->port_handle);
            if (FAILED(hr)) {
                LOG_WARN("WHPX doorbell: WHvDeleteNotificationPort failed: 0x%08lX", hr);
            }
            up->port_handle = nullptr;
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
