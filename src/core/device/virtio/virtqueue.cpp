#include "core/device/virtio/virtqueue.h"
#include <atomic>
#include <cstring>

void VirtQueue::Setup(uint32_t queue_size, const GuestMemMap& mem) {
    queue_size_ = queue_size;
    mem_ = mem;
    last_avail_idx_ = 0;
}

void VirtQueue::Reset() {
    desc_gpa_ = 0;
    driver_gpa_ = 0;
    device_gpa_ = 0;
    last_avail_idx_ = 0;
    last_signalled_used_ = 0;
    ready_ = false;
    event_idx_ = false;
}

uint8_t* VirtQueue::GpaToHva(uint64_t gpa) const {
    return mem_.GpaToHva(gpa);
}

VirtqDesc* VirtQueue::DescAt(uint16_t idx) const {
    if (idx >= queue_size_) return nullptr;
    auto* base = reinterpret_cast<VirtqDesc*>(GpaToHva(desc_gpa_));
    if (!base) return nullptr;
    return &base[idx];
}

VirtqAvail* VirtQueue::Avail() const {
    return reinterpret_cast<VirtqAvail*>(GpaToHva(driver_gpa_));
}

uint16_t* VirtQueue::AvailRing() const {
    auto* avail = Avail();
    if (!avail) return nullptr;
    return reinterpret_cast<uint16_t*>(
        reinterpret_cast<uint8_t*>(avail) + sizeof(VirtqAvail));
}

VirtqUsed* VirtQueue::Used() const {
    return reinterpret_cast<VirtqUsed*>(GpaToHva(device_gpa_));
}

VirtqUsedElem* VirtQueue::UsedRing() const {
    auto* used = Used();
    if (!used) return nullptr;
    return reinterpret_cast<VirtqUsedElem*>(
        reinterpret_cast<uint8_t*>(used) + sizeof(VirtqUsed));
}

bool VirtQueue::HasAvailable() const {
    if (!ready_) return false;
    auto* avail = Avail();
    if (!avail) return false;
    // On ARM64 we need a load-acquire barrier to see guest's latest writes
    // to the avail ring, since the guest CPU is a separate observer.
#if defined(__aarch64__)
    __asm__ volatile("dmb ish" ::: "memory");
#endif
    return last_avail_idx_ != avail->idx;
}

bool VirtQueue::PopAvail(uint16_t* head_idx) {
    if (!HasAvailable()) return false;

    auto* ring = AvailRing();
    if (!ring) return false;

    *head_idx = ring[last_avail_idx_ % queue_size_];
    last_avail_idx_++;

    if (event_idx_) {
        WriteAvailEvent(last_avail_idx_);
    }

    return true;
}

bool VirtQueue::WalkChain(uint16_t head_idx,
                           std::vector<VirtqChainElem>* chain) {
    chain->clear();
    uint16_t idx = head_idx;
    uint32_t count = 0;

    while (count < queue_size_) {
        auto* desc = DescAt(idx);
        if (!desc) {
            LOG_ERROR("VirtQueue: invalid descriptor index %u", idx);
            return false;
        }

        if (desc->flags & VIRTQ_DESC_F_INDIRECT) {
            if (!WalkIndirect(desc->addr, desc->len, chain))
                return false;
        } else {
            uint8_t* hva = GpaToHva(desc->addr);
            if (!hva) {
                LOG_ERROR("VirtQueue: bad GPA 0x%" PRIX64 " in descriptor %u",
                          desc->addr, idx);
                return false;
            }

            chain->push_back({
                hva,
                desc->len,
                (desc->flags & VIRTQ_DESC_F_WRITE) != 0
            });
        }

        if (!(desc->flags & VIRTQ_DESC_F_NEXT))
            break;

        idx = desc->next;
        count++;
    }

    return !chain->empty();
}

bool VirtQueue::WalkIndirect(uint64_t table_gpa, uint32_t table_len,
                              std::vector<VirtqChainElem>* chain) {
    uint32_t num_descs = table_len / sizeof(VirtqDesc);
    if (num_descs == 0 || table_len % sizeof(VirtqDesc) != 0) {
        LOG_ERROR("VirtQueue: invalid indirect table len %u", table_len);
        return false;
    }

    auto* table = reinterpret_cast<VirtqDesc*>(GpaToHva(table_gpa));
    if (!table) {
        LOG_ERROR("VirtQueue: bad GPA 0x%" PRIX64 " for indirect table", table_gpa);
        return false;
    }

    for (uint32_t i = 0; i < num_descs; i++) {
        auto* d = &table[i];

        // Nested indirect is forbidden by spec
        if (d->flags & VIRTQ_DESC_F_INDIRECT) {
            LOG_ERROR("VirtQueue: nested indirect descriptor");
            return false;
        }

        uint8_t* hva = GpaToHva(d->addr);
        if (!hva) {
            LOG_ERROR("VirtQueue: bad GPA 0x%" PRIX64 " in indirect descriptor %u",
                      d->addr, i);
            return false;
        }

        chain->push_back({
            hva,
            d->len,
            (d->flags & VIRTQ_DESC_F_WRITE) != 0
        });

        if (!(d->flags & VIRTQ_DESC_F_NEXT))
            break;
    }

    return true;
}

void VirtQueue::PushUsed(uint16_t head_idx, uint32_t total_len) {
    auto* used = Used();
    if (!used) return;

    auto* ring = UsedRing();
    if (!ring) return;

    uint16_t used_idx = used->idx % queue_size_;
    ring[used_idx].id = head_idx;
    ring[used_idx].len = total_len;

    // Memory barrier: ensure the ring entry is visible before updating idx.
    // The guest vCPU runs on a separate thread, so we need a full hardware
    // barrier on all architectures, not just a compiler fence.
    std::atomic_thread_fence(std::memory_order_release);

    used->idx++;
}

uint16_t VirtQueue::ReadUsedEvent() const {
    auto* ring = AvailRing();
    if (!ring) return 0;
    // used_event sits right after avail->ring[queue_size_]
    return ring[queue_size_];
}

void VirtQueue::WriteAvailEvent(uint16_t val) {
    auto* ring = UsedRing();
    if (!ring) return;
    // avail_event sits right after used->ring[queue_size_]
    auto* avail_event = reinterpret_cast<uint16_t*>(&ring[queue_size_]);
    *avail_event = val;
}

bool VirtQueue::ShouldNotifyGuest() {
    if (!event_idx_) {
        auto* avail = Avail();
        if (avail && (avail->flags & 1))
            return false;
        return true;
    }

    auto* used = Used();
    if (!used) return true;

    uint16_t new_idx = used->idx;

    // Virtio spec 2.7.7.2: the device MUST perform a memory barrier after
    // writing used->idx and before reading used_event, so that the guest's
    // latest used_event write is visible.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    uint16_t used_event = ReadUsedEvent();

    // Notify if used->idx crossed the used_event threshold.
    // Uses unsigned wrap-around arithmetic per virtio spec 2.7.7.2.
    bool notify = static_cast<uint16_t>(new_idx - used_event - 1) <
                  static_cast<uint16_t>(new_idx - last_signalled_used_);
    if (notify)
        last_signalled_used_ = new_idx;
    return notify;
}
