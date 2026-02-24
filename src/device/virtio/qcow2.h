#pragma once

#include "device/virtio/disk_image.h"
#include <cstdio>
#include <vector>
#include <list>
#include <unordered_map>
#include <mutex>

#pragma pack(push, 1)
struct Qcow2Header {
    uint32_t magic;
    uint32_t version;
    uint64_t backing_file_offset;
    uint32_t backing_file_size;
    uint32_t cluster_bits;
    uint64_t size;                   // virtual disk size
    uint32_t crypt_method;
    uint32_t l1_size;
    uint64_t l1_table_offset;
    uint64_t refcount_table_offset;
    uint32_t refcount_table_clusters;
    uint32_t nb_snapshots;
    uint64_t snapshots_offset;
    // v3 extended fields (not used, but present for alignment)
    uint64_t incompatible_features;
    uint64_t compatible_features;
    uint64_t autoclear_features;
    uint32_t refcount_order;         // v3
    uint32_t header_length;          // v3
};
#pragma pack(pop)

class Qcow2DiskImage : public DiskImage {
public:
    ~Qcow2DiskImage() override;

    bool Open(const std::string& path) override;
    uint64_t GetSize() const override { return virtual_size_; }
    bool Read(uint64_t offset, void* buf, uint32_t len) override;
    bool Write(uint64_t offset, const void* buf, uint32_t len) override;
    bool Flush() override;

private:
    static constexpr uint32_t kQcow2Magic   = 0x514649FB;
    static constexpr uint64_t kCompressedBit = 1ULL << 62;
    static constexpr uint64_t kCopiedBit     = 1ULL << 63;
    // Mask to extract the host offset from L1/L2 entries (bits 9..55)
    static constexpr uint64_t kOffsetMask    = 0x00FFFFFFFFFFFE00ULL;
    static constexpr size_t   kL2CacheMax    = 16;

    static uint16_t Be16(uint16_t v);
    static uint32_t Be32(uint32_t v);
    static uint64_t Be64(uint64_t v);

    bool ReadHeader();
    bool ReadL1Table();

    // L2 cache: returns pointer to cached L2 table entries (host byte order).
    // The returned pointer is valid until the next L2 lookup (LRU eviction).
    uint64_t* GetL2Table(uint64_t l2_offset);
    void EvictL2Cache();

    // Resolve a virtual offset to a host file offset. Returns 0 if unallocated.
    // Sets `compressed` and `comp_size` if the cluster is compressed.
    uint64_t ResolveOffset(uint64_t virt_offset, bool* compressed,
                           uint64_t* comp_host_off, uint32_t* comp_size);

    // Allocate a new cluster at the end of the file.
    uint64_t AllocateCluster();
    uint64_t AllocateClusters(uint32_t count);

    // Ensure L2 table is allocated for the given L1 index.
    uint64_t* EnsureL2Table(uint32_t l1_idx);

    bool ReadCluster(uint64_t host_off, uint64_t in_cluster_off,
                     void* buf, uint32_t len);
    bool ReadCompressedCluster(uint64_t comp_host_off, uint32_t comp_size,
                               uint64_t in_cluster_off, void* buf, uint32_t len);
    bool WriteCluster(uint64_t host_off, uint64_t in_cluster_off,
                      const void* buf, uint32_t len);

    FILE* file_ = nullptr;
    uint64_t virtual_size_ = 0;
    uint32_t cluster_bits_ = 0;
    uint32_t cluster_size_ = 0;
    uint32_t l2_entries_ = 0;        // entries per L2 table
    uint32_t l1_size_ = 0;
    uint64_t l1_table_offset_ = 0;
    uint32_t version_ = 0;

    std::vector<uint64_t> l1_table_;  // in host byte order
    uint64_t file_end_ = 0;          // current end of file (for append allocations)

    // L2 LRU cache
    struct L2CacheEntry {
        uint64_t l2_offset;           // file offset of this L2 table
        std::vector<uint64_t> data;   // entries in host byte order
        bool dirty;
    };
    std::list<L2CacheEntry> l2_lru_;
    std::unordered_map<uint64_t, std::list<L2CacheEntry>::iterator> l2_map_;
};
