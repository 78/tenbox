#pragma once

#include "vmm/types.h"
#include <cstdint>
#include <vector>

// VirtIO split virtqueue descriptor (16 bytes each, in guest memory).
#pragma pack(push, 1)
struct VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#pragma pack(pop)

static_assert(sizeof(VirtqDesc) == 16);

// Descriptor flags
constexpr uint16_t VIRTQ_DESC_F_NEXT     = 1;
constexpr uint16_t VIRTQ_DESC_F_WRITE    = 2;
constexpr uint16_t VIRTQ_DESC_F_INDIRECT = 4;

// Available ring header (in guest memory).
#pragma pack(push, 1)
struct VirtqAvail {
    uint16_t flags;
    uint16_t idx;
    // Followed by uint16_t ring[queue_size] and optionally uint16_t used_event.
};
#pragma pack(pop)

// Used ring element.
#pragma pack(push, 1)
struct VirtqUsedElem {
    uint32_t id;
    uint32_t len;
};
#pragma pack(pop)

// Used ring header (in guest memory).
#pragma pack(push, 1)
struct VirtqUsed {
    uint16_t flags;
    uint16_t idx;
    // Followed by VirtqUsedElem ring[queue_size] and optionally uint16_t avail_event.
};
#pragma pack(pop)

// One element of a descriptor chain, already translated to HVA.
struct VirtqChainElem {
    uint8_t* addr;
    uint32_t len;
    bool     writable;
};

class VirtQueue {
public:
    void Setup(uint32_t queue_size, uint8_t* ram, uint64_t ram_size);

    void SetDescAddr(uint64_t gpa)   { desc_gpa_ = gpa; }
    void SetDriverAddr(uint64_t gpa) { driver_gpa_ = gpa; }
    void SetDeviceAddr(uint64_t gpa) { device_gpa_ = gpa; }
    void SetReady(bool ready)        { ready_ = ready; }
    bool IsReady() const             { return ready_; }
    uint32_t Size() const            { return queue_size_; }

    void Reset();

    bool HasAvailable() const;

    // Pop the next available descriptor chain head index.
    // Returns false if no buffers are available.
    bool PopAvail(uint16_t* head_idx);

    // Walk a descriptor chain starting at head_idx, collecting all elements.
    bool WalkChain(uint16_t head_idx, std::vector<VirtqChainElem>* chain);

    // Push a completed buffer to the used ring.
    void PushUsed(uint16_t head_idx, uint32_t total_len);

private:
    uint8_t* GpaToHva(uint64_t gpa) const;

    VirtqDesc* DescAt(uint16_t idx) const;
    uint16_t* AvailRing() const;
    VirtqUsedElem* UsedRing() const;
    VirtqAvail* Avail() const;
    VirtqUsed* Used() const;

    uint32_t queue_size_ = 0;
    uint8_t* ram_ = nullptr;
    uint64_t ram_size_ = 0;

    uint64_t desc_gpa_ = 0;
    uint64_t driver_gpa_ = 0;
    uint64_t device_gpa_ = 0;

    uint16_t last_avail_idx_ = 0;
    bool ready_ = false;
};
