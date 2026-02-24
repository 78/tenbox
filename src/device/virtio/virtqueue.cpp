#include "device/virtio/virtqueue.h"
#include <cstring>

void VirtQueue::Setup(uint32_t queue_size, uint8_t* ram, uint64_t ram_size) {
    queue_size_ = queue_size;
    ram_ = ram;
    ram_size_ = ram_size;
    last_avail_idx_ = 0;
}

void VirtQueue::Reset() {
    desc_gpa_ = 0;
    driver_gpa_ = 0;
    device_gpa_ = 0;
    last_avail_idx_ = 0;
    ready_ = false;
}

uint8_t* VirtQueue::GpaToHva(uint64_t gpa) const {
    if (gpa >= ram_size_) return nullptr;
    return ram_ + gpa;
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
    return last_avail_idx_ != avail->idx;
}

bool VirtQueue::PopAvail(uint16_t* head_idx) {
    if (!HasAvailable()) return false;

    auto* ring = AvailRing();
    if (!ring) return false;

    *head_idx = ring[last_avail_idx_ % queue_size_];
    last_avail_idx_++;
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

        uint8_t* hva = GpaToHva(desc->addr);
        if (!hva) {
            LOG_ERROR("VirtQueue: bad GPA 0x%llX in descriptor %u",
                      desc->addr, idx);
            return false;
        }

        chain->push_back({
            hva,
            desc->len,
            (desc->flags & VIRTQ_DESC_F_WRITE) != 0
        });

        if (!(desc->flags & VIRTQ_DESC_F_NEXT))
            break;

        idx = desc->next;
        count++;
    }

    return !chain->empty();
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
    _ReadWriteBarrier();

    used->idx++;
}
