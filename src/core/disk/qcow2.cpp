#include "core/disk/qcow2.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <intrin.h>
#include <io.h>       // _commit, _fileno
#else
#include <unistd.h>   // fsync, fileno
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

// zlib for compressed cluster support
#include <zlib.h>
// zstd for qcow2 zstd compressed cluster support
#include <zstd.h>

// ---------- byte-swap helpers (big-endian on disk) ----------

uint16_t Qcow2DiskImage::Be16(uint16_t v) {
#ifdef _WIN32
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}

uint32_t Qcow2DiskImage::Be32(uint32_t v) {
#ifdef _WIN32
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

uint64_t Qcow2DiskImage::Be64(uint64_t v) {
#ifdef _WIN32
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

// ---------- lifecycle ----------

Qcow2DiskImage::~Qcow2DiskImage() {
    if (file_) {
        Flush();
        ClearDirtyBit();
        fclose(file_);
        file_ = nullptr;
    }
}

bool Qcow2DiskImage::Open(const std::string& path) {
    file_ = fopen(path.c_str(), "r+b");
    if (!file_) {
        LOG_ERROR("Qcow2: failed to open %s", path.c_str());
        return false;
    }

    if (!AcquireExclusiveLock(file_, path)) {
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    if (!ReadHeader()) {
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    if (!ReadL1Table()) {
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    if (!ReadRefcountTable()) {
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    // Determine file end for physical file extension tracking
    _fseeki64(file_, 0, SEEK_END);
    file_end_ = static_cast<uint64_t>(_ftelli64(file_));
    // Align to cluster boundary
    file_end_ = (file_end_ + cluster_size_ - 1) & ~(static_cast<uint64_t>(cluster_size_) - 1);

    LOG_INFO("Qcow2: %s, version %u, cluster_size %u, virtual_size %" PRIu64
             " MB, l1_size %u, refcount_table 0x%" PRIX64
             " (%u clusters), file_end 0x%" PRIX64 ", compression %s",
             path.c_str(), version_, cluster_size_,
             virtual_size_ / (1024 * 1024), l1_size_,
             refcount_table_offset_, refcount_table_clusters_,
             file_end_,
             compression_type_ == 1 ? "zstd" : "zlib");
    
    RepairLeaks(true);
    SetDirtyBit();

    return true;
}

// ---------- header parsing ----------

bool Qcow2DiskImage::ReadHeader() {
    Qcow2Header hdr{};
    _fseeki64(file_, 0, SEEK_SET);
    if (fread(&hdr, 1, sizeof(hdr), file_) < 72) {
        LOG_ERROR("Qcow2: header too short");
        return false;
    }

    if (Be32(hdr.magic) != kQcow2Magic) {
        LOG_ERROR("Qcow2: bad magic 0x%08X", Be32(hdr.magic));
        return false;
    }

    version_ = Be32(hdr.version);
    if (version_ != 2 && version_ != 3) {
        LOG_ERROR("Qcow2: unsupported version %u", version_);
        return false;
    }

    if (Be64(hdr.backing_file_offset) != 0) {
        LOG_ERROR("Qcow2: backing files not supported");
        return false;
    }

    if (Be32(hdr.crypt_method) != 0) {
        LOG_ERROR("Qcow2: encrypted images not supported");
        return false;
    }

    cluster_bits_ = Be32(hdr.cluster_bits);
    if (cluster_bits_ < 9 || cluster_bits_ > 21) {
        LOG_ERROR("Qcow2: invalid cluster_bits %u", cluster_bits_);
        return false;
    }

    cluster_size_ = 1u << cluster_bits_;
    l2_entries_ = cluster_size_ / 8;  // each L2 entry is 8 bytes
    virtual_size_ = Be64(hdr.size);
    l1_size_ = Be32(hdr.l1_size);
    l1_table_offset_ = Be64(hdr.l1_table_offset);

    refcount_table_offset_ = Be64(hdr.refcount_table_offset);
    refcount_table_clusters_ = Be32(hdr.refcount_table_clusters);

    // v3 feature bits & extended fields
    refcount_order_ = 4;
    compression_type_ = 0;
    if (version_ == 3) {
        // Per spec: "An implementation must fail to open an image if an
        // unknown incompatible feature bit is set."
        // We support: bit 0 (dirty) and bit 3 (compression type).
        constexpr uint64_t kSupportedIncompat =
            (1ULL << 0) |   // dirty
            (1ULL << 3);    // compression type
        uint64_t incompat = Be64(hdr.incompatible_features);
        if (incompat & ~kSupportedIncompat) {
            LOG_ERROR("Qcow2: unsupported incompatible features 0x%" PRIX64
                      " (supported mask 0x%" PRIX64 ")",
                      incompat, kSupportedIncompat);
            return false;
        }
        if (incompat & (1ULL << 0)) {
            LOG_INFO("Qcow2: dirty bit set — will repair refcounts on open");
        }

        refcount_order_ = Be32(hdr.refcount_order);
        if (refcount_order_ > 6) {
            LOG_ERROR("Qcow2: invalid refcount_order %u (max 6)", refcount_order_);
            return false;
        }

        uint32_t header_length = Be32(hdr.header_length);
        if (header_length > offsetof(Qcow2Header, refcount_order) + 8) {
            uint8_t comp_type = 0;
            _fseeki64(file_, 104, SEEK_SET);
            if (fread(&comp_type, 1, 1, file_) == 1) {
                compression_type_ = comp_type;
                if (compression_type_ > 1) {
                    LOG_ERROR("Qcow2: unsupported compression_type %u",
                              compression_type_);
                    return false;
                }
            }
        }
    }

    refcount_bits_ = 1u << refcount_order_;
    if (refcount_bits_ != 16) {
        LOG_ERROR("Qcow2: unsupported refcount_bits %u (only 16-bit supported)",
                  refcount_bits_);
        return false;
    }
    rfb_entries_ = cluster_size_ * 8 / refcount_bits_;

    return true;
}

void Qcow2DiskImage::SetDirtyBit() {
    if (version_ < 3) return;
    uint64_t incompat;
    _fseeki64(file_, offsetof(Qcow2Header, incompatible_features), SEEK_SET);
    if (fread(&incompat, sizeof(incompat), 1, file_) != 1) return;
    incompat = Be64(Be64(incompat) | (1ULL << 0));
    _fseeki64(file_, offsetof(Qcow2Header, incompatible_features), SEEK_SET);
    fwrite(&incompat, sizeof(incompat), 1, file_);
    fflush(file_);
}

void Qcow2DiskImage::ClearDirtyBit() {
    if (version_ < 3) return;
    uint64_t incompat;
    _fseeki64(file_, offsetof(Qcow2Header, incompatible_features), SEEK_SET);
    if (fread(&incompat, sizeof(incompat), 1, file_) != 1) return;
    incompat = Be64(Be64(incompat) & ~(1ULL << 0));
    _fseeki64(file_, offsetof(Qcow2Header, incompatible_features), SEEK_SET);
    fwrite(&incompat, sizeof(incompat), 1, file_);
    fflush(file_);
}

bool Qcow2DiskImage::ReadL1Table() {
    l1_table_.resize(l1_size_);
    _fseeki64(file_, l1_table_offset_, SEEK_SET);
    size_t bytes = l1_size_ * sizeof(uint64_t);
    if (fread(l1_table_.data(), 1, bytes, file_) != bytes) {
        LOG_ERROR("Qcow2: failed to read L1 table (%u entries at 0x%" PRIX64 ")",
                  l1_size_, l1_table_offset_);
        return false;
    }

    // Convert from big-endian to host byte order
    for (uint32_t i = 0; i < l1_size_; i++) {
        l1_table_[i] = Be64(l1_table_[i]);
    }
    return true;
}

bool Qcow2DiskImage::ReadRefcountTable() {
    size_t table_bytes = static_cast<size_t>(refcount_table_clusters_) * cluster_size_;
    size_t table_entries = table_bytes / sizeof(uint64_t);
    refcount_table_.resize(table_entries);

    _fseeki64(file_, refcount_table_offset_, SEEK_SET);
    if (fread(refcount_table_.data(), 1, table_bytes, file_) != table_bytes) {
        LOG_ERROR("Qcow2: failed to read refcount table at 0x%" PRIX64 " (%zu bytes)",
                  refcount_table_offset_, table_bytes);
        return false;
    }

    for (size_t i = 0; i < table_entries; i++) {
        uint64_t raw = Be64(refcount_table_[i]);
        uint64_t masked = raw & kReftOffsetMask;
        if (masked != raw)
            refcount_table_dirty_ = true;
        refcount_table_[i] = masked;
    }
    return true;
}

// ---------- L2 cache ----------

uint64_t* Qcow2DiskImage::GetL2Table(uint64_t l2_offset) {
    auto it = l2_map_.find(l2_offset);
    if (it != l2_map_.end()) {
        // Move to front (most recently used)
        l2_lru_.splice(l2_lru_.begin(), l2_lru_, it->second);
        return it->second->data.data();
    }

    // Evict if cache full
    while (l2_lru_.size() >= kL2CacheMax) {
        EvictL2Cache();
    }

    // Read from disk
    L2CacheEntry entry;
    entry.l2_offset = l2_offset;
    entry.data.resize(l2_entries_);
    entry.dirty = false;

    _fseeki64(file_, l2_offset, SEEK_SET);
    size_t bytes = l2_entries_ * sizeof(uint64_t);
    if (fread(entry.data.data(), 1, bytes, file_) != bytes) {
        LOG_ERROR("Qcow2: failed to read L2 table at 0x%" PRIX64, l2_offset);
        return nullptr;
    }

    // Convert to host byte order
    for (uint32_t i = 0; i < l2_entries_; i++) {
        entry.data[i] = Be64(entry.data[i]);
    }

    l2_lru_.push_front(std::move(entry));
    l2_map_[l2_offset] = l2_lru_.begin();
    return l2_lru_.front().data.data();
}

void Qcow2DiskImage::EvictL2Cache() {
    if (l2_lru_.empty()) return;

    auto& victim = l2_lru_.back();
    if (victim.dirty) {
        std::vector<uint64_t> be_data(l2_entries_);
        for (uint32_t i = 0; i < l2_entries_; i++) {
            be_data[i] = Be64(victim.data[i]);
        }
        size_t bytes = l2_entries_ * sizeof(uint64_t);
        _fseeki64(file_, victim.l2_offset, SEEK_SET);
        if (fwrite(be_data.data(), 1, bytes, file_) != bytes) {
            LOG_ERROR("Qcow2: failed to evict L2 table at 0x%" PRIX64, victim.l2_offset);
        }
    }

    l2_map_.erase(victim.l2_offset);
    l2_lru_.pop_back();
}

// ---------- refcount block cache ----------

uint16_t* Qcow2DiskImage::GetRefcountBlock(uint64_t cluster_index,
                                             uint32_t* rfb_index,
                                             bool allocate) {
    uint64_t rft_index = cluster_index / rfb_entries_;
    *rfb_index = static_cast<uint32_t>(cluster_index % rfb_entries_);

    if (rft_index >= refcount_table_.size()) {
        if (!allocate) return nullptr;
        if (!GrowRefcountTable(cluster_index)) {
            LOG_ERROR("Qcow2: failed to grow refcount table for cluster %" PRIu64, cluster_index);
            return nullptr;
        }
        // rft_index is now valid after grow
    }

    uint64_t block_offset = refcount_table_[rft_index];

    if (block_offset == 0) {
        if (!allocate) return nullptr;

        // Use this cluster position as the refcount block itself
        block_offset = static_cast<uint64_t>(cluster_index) << cluster_bits_;

        if (block_offset + cluster_size_ > file_end_) {
            file_end_ = block_offset + cluster_size_;
        }
        std::vector<uint8_t> zeros(cluster_size_, 0);
        _fseeki64(file_, block_offset, SEEK_SET);
        if (fwrite(zeros.data(), 1, cluster_size_, file_) != cluster_size_) {
            LOG_ERROR("Qcow2: failed to write new refcount block at 0x%" PRIX64, block_offset);
            return nullptr;
        }

        while (rfb_lru_.size() >= kRfbCacheMax) {
            EvictRfbCache();
        }

        RfbCacheEntry entry;
        entry.offset_in_file = block_offset;
        entry.data.resize(rfb_entries_, 0);
        entry.data[*rfb_index] = 1;  // self-referencing: the block itself uses this cluster
        entry.dirty = true;

        rfb_lru_.push_front(std::move(entry));
        rfb_map_[block_offset] = rfb_lru_.begin();

        refcount_table_[rft_index] = block_offset;
        refcount_table_dirty_ = true;

        return rfb_lru_.front().data.data();
    }

    // Look up in cache
    auto it = rfb_map_.find(block_offset);
    if (it != rfb_map_.end()) {
        rfb_lru_.splice(rfb_lru_.begin(), rfb_lru_, it->second);
        return it->second->data.data();
    }

    while (rfb_lru_.size() >= kRfbCacheMax) {
        EvictRfbCache();
    }

    RfbCacheEntry entry;
    entry.offset_in_file = block_offset;
    entry.data.resize(rfb_entries_);
    entry.dirty = false;

    _fseeki64(file_, block_offset, SEEK_SET);
    size_t bytes = rfb_entries_ * sizeof(uint16_t);
    if (fread(entry.data.data(), 1, bytes, file_) != bytes) {
        LOG_ERROR("Qcow2: failed to read refcount block at 0x%" PRIX64, block_offset);
        return nullptr;
    }

    for (uint32_t i = 0; i < rfb_entries_; i++) {
        entry.data[i] = Be16(entry.data[i]);
    }

    rfb_lru_.push_front(std::move(entry));
    rfb_map_[block_offset] = rfb_lru_.begin();
    return rfb_lru_.front().data.data();
}

void Qcow2DiskImage::EvictRfbCache() {
    if (rfb_lru_.empty()) return;

    auto& victim = rfb_lru_.back();
    if (victim.dirty) {
        std::vector<uint16_t> be_data(rfb_entries_);
        for (uint32_t i = 0; i < rfb_entries_; i++) {
            be_data[i] = Be16(victim.data[i]);
        }
        size_t bytes = rfb_entries_ * sizeof(uint16_t);
        _fseeki64(file_, victim.offset_in_file, SEEK_SET);
        if (fwrite(be_data.data(), 1, bytes, file_) != bytes) {
            LOG_ERROR("Qcow2: failed to evict refcount block at 0x%" PRIX64,
                      victim.offset_in_file);
        }
    }

    rfb_map_.erase(victim.offset_in_file);
    rfb_lru_.pop_back();
}

void Qcow2DiskImage::FlushRefcountTable() {
    std::vector<uint64_t> be_table(refcount_table_.size());
    for (size_t i = 0; i < refcount_table_.size(); i++) {
        be_table[i] = Be64(refcount_table_[i]);
    }
    _fseeki64(file_, refcount_table_offset_, SEEK_SET);
    if (fwrite(be_table.data(), 1, be_table.size() * sizeof(uint64_t), file_)
        != be_table.size() * sizeof(uint64_t)) {
        LOG_ERROR("Qcow2: failed to write refcount table");
    }
    refcount_table_dirty_ = false;
}

bool Qcow2DiskImage::GrowRefcountTable(uint64_t min_cluster_index) {
    Flush();

    uint64_t needed_rft_index = min_cluster_index / rfb_entries_;

    // The new table is placed at file_end_.  We must size it large enough
    // to cover both the requested cluster AND the table's own clusters, plus
    // one refcount block per table entry range that doesn't have one yet.
    // Iterate until the table is self-covering.
    uint64_t new_table_offset = file_end_;
    size_t new_entries;
    size_t new_byte_size;
    uint32_t new_clusters;

    size_t min_entries = static_cast<size_t>(needed_rft_index + 1);
    for (;;) {
        new_entries = min_entries * 3 / 2;  // 150% headroom
        new_byte_size = new_entries * sizeof(uint64_t);
        new_clusters =
            static_cast<uint32_t>((new_byte_size + cluster_size_ - 1) / cluster_size_);
        new_entries = static_cast<size_t>(new_clusters) * cluster_size_ / sizeof(uint64_t);
        new_byte_size = new_entries * sizeof(uint64_t);

        // Check: can this table cover its own last cluster?
        uint64_t table_last_ci =
            (new_table_offset + new_byte_size - 1) >> cluster_bits_;
        uint64_t needed_rft = table_last_ci / rfb_entries_ + 1;
        if (needed_rft <= new_entries) break;
        min_entries = static_cast<size_t>(needed_rft);
    }

    file_end_ = new_table_offset + new_byte_size;

    uint64_t old_table_offset = refcount_table_offset_;
    uint32_t old_table_clusters = refcount_table_clusters_;

    // Build the new table: copy old entries, zero-fill the rest.
    std::vector<uint64_t> new_table(new_entries, 0);
    for (size_t i = 0; i < refcount_table_.size() && i < new_entries; i++) {
        new_table[i] = refcount_table_[i];
    }

    // Write new table to disk (big-endian).
    std::vector<uint64_t> be_table(new_entries);
    for (size_t i = 0; i < new_entries; i++) {
        be_table[i] = Be64(new_table[i]);
    }
    _fseeki64(file_, new_table_offset, SEEK_SET);
    if (fwrite(be_table.data(), 1, new_byte_size, file_) != new_byte_size) {
        LOG_ERROR("Qcow2: failed to write new refcount table at 0x%" PRIX64, new_table_offset);
        return false;
    }

    // Update header: refcount_table_offset (offset 48) and
    // refcount_table_clusters (offset 56).
    uint64_t be_offset = Be64(new_table_offset);
    _fseeki64(file_, 48, SEEK_SET);
    if (fwrite(&be_offset, sizeof(be_offset), 1, file_) != 1) {
        LOG_ERROR("Qcow2: failed to update refcount_table_offset in header");
        return false;
    }
    uint32_t be_nclusters = Be32(new_clusters);
    _fseeki64(file_, 56, SEEK_SET);
    if (fwrite(&be_nclusters, sizeof(be_nclusters), 1, file_) != 1) {
        LOG_ERROR("Qcow2: failed to update refcount_table_clusters in header");
        return false;
    }
    fflush(file_);

    // Switch in-memory state.
    refcount_table_ = std::move(new_table);
    refcount_table_offset_ = new_table_offset;
    refcount_table_clusters_ = new_clusters;
    refcount_table_dirty_ = false;

    // Invalidate in-memory refcount block cache: we're about to manipulate
    // refcount blocks directly on disk for the new table's own clusters.
    // This avoids stale cache entries conflicting with our direct writes.
    for (auto& entry : rfb_lru_) {
        if (entry.dirty) {
            std::vector<uint16_t> be_data(rfb_entries_);
            for (uint32_t i = 0; i < rfb_entries_; i++)
                be_data[i] = Be16(entry.data[i]);
            size_t bytes = rfb_entries_ * sizeof(uint16_t);
            _fseeki64(file_, entry.offset_in_file, SEEK_SET);
            fwrite(be_data.data(), 1, bytes, file_);
        }
    }
    rfb_lru_.clear();
    rfb_map_.clear();

    // Set refcount=1 for each cluster occupied by the new table.
    // We write directly to refcount blocks on disk to avoid recursive
    // GetRefcountBlock → GrowRefcountTable calls.
    //
    // Phase A: set refcount=1 for each table cluster, creating new
    // refcount blocks as needed and tracking them.
    std::vector<uint64_t> new_block_offsets;

    auto ensure_block = [&](uint64_t rft_i) -> uint64_t {
        uint64_t block_off = refcount_table_[rft_i];
        if (block_off != 0) return block_off;

        block_off = file_end_;
        file_end_ += cluster_size_;

        std::vector<uint8_t> zeros(cluster_size_, 0);
        _fseeki64(file_, block_off, SEEK_SET);
        fwrite(zeros.data(), 1, cluster_size_, file_);

        refcount_table_[rft_i] = block_off;
        refcount_table_dirty_ = true;
        new_block_offsets.push_back(block_off);
        return block_off;
    };

    auto set_refcount_1 = [&](uint64_t ci) {
        uint64_t rft_i = ci / rfb_entries_;
        uint32_t rfb_i = static_cast<uint32_t>(ci % rfb_entries_);
        uint64_t block_off = ensure_block(rft_i);
        uint16_t one = Be16(1);
        _fseeki64(file_, block_off + rfb_i * sizeof(uint16_t), SEEK_SET);
        fwrite(&one, sizeof(one), 1, file_);
    };

    for (uint32_t i = 0; i < new_clusters; i++) {
        set_refcount_1((new_table_offset >> cluster_bits_) + i);
    }

    // Phase B: set refcount=1 for every newly created refcount block.
    // This may cascade (a block's cluster may need yet another block),
    // so we iterate until all are handled.
    for (size_t bi = 0; bi < new_block_offsets.size(); bi++) {
        set_refcount_1(new_block_offsets[bi] >> cluster_bits_);
    }

    // Flush updated refcount table to disk.
    if (refcount_table_dirty_) {
        FlushRefcountTable();
    }
    fflush(file_);

    // Free the old refcount table clusters.
    // Use GetRefcountBlock normally now (cache was invalidated, state is
    // consistent after our direct writes).
    for (uint32_t i = 0; i < old_table_clusters; i++) {
        FreeCluster(old_table_offset + static_cast<uint64_t>(i) * cluster_size_);
    }

    LOG_INFO("Qcow2: grew refcount table to %u clusters (%zu entries) at 0x%" PRIX64,
             new_clusters, new_entries, new_table_offset);
    return true;
}

// ---------- offset resolution ----------

uint64_t Qcow2DiskImage::ResolveOffset(uint64_t virt_offset, bool* compressed,
                                         uint64_t* comp_host_off,
                                         uint32_t* comp_size) {
    *compressed = false;
    *comp_host_off = 0;
    *comp_size = 0;

    uint32_t l1_idx = static_cast<uint32_t>(
        virt_offset / (static_cast<uint64_t>(l2_entries_) * cluster_size_));
    uint32_t l2_idx = static_cast<uint32_t>(
        (virt_offset / cluster_size_) % l2_entries_);

    if (l1_idx >= l1_size_) return 0;

    uint64_t l1_entry = l1_table_[l1_idx];
    if (l1_entry == 0) return 0;

    uint64_t l2_table_off = l1_entry & kOffsetMask;
    uint64_t* l2 = GetL2Table(l2_table_off);
    if (!l2) return 0;

    uint64_t l2_entry = l2[l2_idx];
    if (l2_entry == 0) return 0;

    if (l2_entry & kCompressedBit) {
        *compressed = true;
        // For compressed clusters (QEMU qcow2 format):
        // csize_shift = 62 - (cluster_bits - 8)
        // Bits 0 to (csize_shift - 1): host offset
        // Bits csize_shift to 61: compressed sectors - 1
        // Bit 62: compressed flag
        // Bit 63: copied flag (unused for compressed)
        uint32_t csize_shift = 62 - (cluster_bits_ - 8);
        uint64_t csize_mask = (1ULL << (cluster_bits_ - 8)) - 1;
        uint64_t offset_mask = (1ULL << csize_shift) - 1;

        uint32_t nb_csectors = static_cast<uint32_t>(
            ((l2_entry >> csize_shift) & csize_mask) + 1);
        uint64_t host_off = l2_entry & offset_mask;

        *comp_host_off = host_off;
        *comp_size = nb_csectors * 512 - (host_off & 511);
        return 0;  // caller must use compressed path
    }

    // v3 zero flag: bit 0 means cluster reads as all zeros even if a host
    // offset is present (used for preallocation).
    if (version_ >= 3 && (l2_entry & kZeroFlag)) {
        return 0;
    }

    return l2_entry & kOffsetMask;
}

// ---------- cluster I/O ----------

bool Qcow2DiskImage::ReadCluster(uint64_t host_off, uint64_t in_cluster_off,
                                   void* buf, uint32_t len) {
    _fseeki64(file_, host_off + in_cluster_off, SEEK_SET);
    return fread(buf, 1, len, file_) == len;
}

bool Qcow2DiskImage::ReadCompressedCluster(uint64_t comp_host_off,
                                             uint32_t comp_size,
                                             uint64_t in_cluster_off,
                                             void* buf, uint32_t len) {
    // Read compressed data
    std::vector<uint8_t> comp_buf(comp_size);
    _fseeki64(file_, comp_host_off, SEEK_SET);
    if (fread(comp_buf.data(), 1, comp_size, file_) != comp_size) {
        LOG_ERROR("Qcow2: failed to read compressed data at 0x%" PRIX64 " (%u bytes)",
                  comp_host_off, comp_size);
        return false;
    }

    // Decompress full cluster
    std::vector<uint8_t> decompressed(cluster_size_);

    if (compression_type_ == 1) {
        // zstd streaming decompression (handles multiple frames)
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        if (!dctx) {
            LOG_ERROR("Qcow2: ZSTD_createDCtx failed");
            return false;
        }
        ZSTD_inBuffer input = { comp_buf.data(), comp_size, 0 };
        ZSTD_outBuffer output = { decompressed.data(), cluster_size_, 0 };

        while (output.pos < output.size) {
            size_t ret = ZSTD_decompressStream(dctx, &output, &input);
            if (ZSTD_isError(ret)) {
                LOG_ERROR("Qcow2: ZSTD_decompressStream failed: %s",
                          ZSTD_getErrorName(ret));
                ZSTD_freeDCtx(dctx);
                return false;
            }
            if (ret == 0 && output.pos < output.size) {
                break;  // no more input data
            }
        }
        ZSTD_freeDCtx(dctx);
    } else if (compression_type_ == 0) {
        // Raw deflate (no zlib header) per QCOW2 spec
        z_stream strm{};
        strm.avail_in = comp_size;
        strm.next_in = comp_buf.data();
        strm.avail_out = cluster_size_;
        strm.next_out = decompressed.data();

        int ret = inflateInit2(&strm, -15);
        if (ret != Z_OK) {
            LOG_ERROR("Qcow2: inflateInit2 failed (%d)", ret);
            return false;
        }
        ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);

        if (ret != Z_STREAM_END && ret != Z_OK) {
            LOG_ERROR("Qcow2: inflate failed (%d) for cluster at 0x%" PRIX64,
                      ret, comp_host_off);
            return false;
        }
    } else {
        LOG_ERROR("Qcow2: unknown compression_type %u", compression_type_);
        return false;
    }

    if (in_cluster_off + len > cluster_size_) {
        LOG_ERROR("Qcow2: read past cluster boundary");
        return false;
    }

    memcpy(buf, decompressed.data() + in_cluster_off, len);
    return true;
}

bool Qcow2DiskImage::WriteCluster(uint64_t host_off, uint64_t in_cluster_off,
                                    const void* buf, uint32_t len) {
    if (!CheckMetadataOverlap(host_off, cluster_size_)) {
        return false;
    }
    _fseeki64(file_, host_off + in_cluster_off, SEEK_SET);
    return fwrite(buf, 1, len, file_) == len;
}

bool Qcow2DiskImage::CheckMetadataOverlap(uint64_t offset, uint64_t size) {
    uint64_t end = offset + size;

    // Header cluster (cluster 0)
    if (offset < cluster_size_) {
        LOG_ERROR("Qcow2: overlap with header at offset 0x%" PRIX64, offset);
        return false;
    }

    // Active L1 table
    uint64_t l1_end = l1_table_offset_ +
        static_cast<uint64_t>(l1_size_) * sizeof(uint64_t);
    if (offset < l1_end && end > l1_table_offset_) {
        LOG_ERROR("Qcow2: overlap with L1 table at offset 0x%" PRIX64, offset);
        return false;
    }

    // Refcount table
    uint64_t rft_end = refcount_table_offset_ +
        static_cast<uint64_t>(refcount_table_clusters_) * cluster_size_;
    if (offset < rft_end && end > refcount_table_offset_) {
        LOG_ERROR("Qcow2: overlap with refcount table at offset 0x%" PRIX64, offset);
        return false;
    }

    // Refcount block that covers THIS cluster (O(1) check instead of
    // scanning all blocks — sufficient since AllocateCluster guarantees
    // the cluster's refcount was 0 before allocation).
    uint64_t ci = offset >> cluster_bits_;
    uint64_t rft_i = ci / rfb_entries_;
    if (rft_i < refcount_table_.size()) {
        uint64_t blk = refcount_table_[rft_i];
        if (blk != 0 && offset < blk + cluster_size_ && end > blk) {
            LOG_ERROR("Qcow2: overlap with refcount block at offset 0x%" PRIX64,
                      offset);
            return false;
        }
    }

    return true;
}

// ---------- allocation ----------

uint64_t Qcow2DiskImage::AllocateCluster() {
    uint32_t rfb_index;
    uint64_t cluster_index = free_cluster_index_++;
    uint16_t* rfb = GetRefcountBlock(cluster_index, &rfb_index, true);

    while (rfb) {
        if (rfb[rfb_index] == 0) {
            break;
        }
        uint64_t prev = cluster_index;
        cluster_index = free_cluster_index_++;
        // Reload the refcount block whenever cluster_index didn't
        // advance by exactly 1 (free_cluster_index_ may have been
        // reset by FreeCluster inside GrowRefcountTable) or when
        // crossing a block boundary.
        if (cluster_index != prev + 1 || ++rfb_index >= rfb_entries_) {
            rfb = GetRefcountBlock(cluster_index, &rfb_index, true);
        }
    }

    if (!rfb) {
        LOG_ERROR("Qcow2: failed to allocate cluster (refcount table exhausted)");
        return 0;
    }

    rfb[rfb_index] = 1;

    // Mark the refcount block dirty
    uint64_t rft_index = cluster_index / rfb_entries_;
    uint64_t block_offset = refcount_table_[rft_index];
    auto it = rfb_map_.find(block_offset);
    if (it != rfb_map_.end()) {
        it->second->dirty = true;
    }

    uint64_t offset = static_cast<uint64_t>(cluster_index) << cluster_bits_;

    // Only zero-fill when extending the file beyond its current end.
    // Clusters within the existing file extent were either previously zeroed
    // or will be fully overwritten by the caller.
    if (offset + cluster_size_ > file_end_) {
        std::vector<uint8_t> zeros(cluster_size_, 0);
        _fseeki64(file_, offset, SEEK_SET);
        if (fwrite(zeros.data(), 1, cluster_size_, file_) != cluster_size_) {
            LOG_ERROR("Qcow2: failed to extend file at 0x%" PRIX64, offset);
            return 0;
        }
        file_end_ = offset + cluster_size_;
    }

    return offset;
}

void Qcow2DiskImage::FreeCluster(uint64_t host_offset) {
    uint64_t cluster_index = host_offset >> cluster_bits_;
    uint32_t rfb_index;
    uint16_t* rfb = GetRefcountBlock(cluster_index, &rfb_index, false);
    if (!rfb) {
        LOG_ERROR("Qcow2: FreeCluster: no refcount block for offset 0x%" PRIX64,
                  host_offset);
        return;
    }

    if (rfb[rfb_index] == 0) {
        LOG_ERROR("Qcow2: FreeCluster: refcount already 0 for offset 0x%" PRIX64,
                  host_offset);
        return;
    }

    rfb[rfb_index]--;

    uint64_t rft_index = cluster_index / rfb_entries_;
    uint64_t block_offset = refcount_table_[rft_index];
    auto it = rfb_map_.find(block_offset);
    if (it != rfb_map_.end()) {
        it->second->dirty = true;
    }

    if (rfb[rfb_index] == 0 && cluster_index < free_cluster_index_) {
        free_cluster_index_ = cluster_index;
    }
}

void Qcow2DiskImage::FreeCompressedCluster(uint64_t l2_entry) {
    uint32_t csize_shift = 62 - (cluster_bits_ - 8);
    uint64_t csize_mask = (1ULL << (cluster_bits_ - 8)) - 1;
    uint64_t off_mask = (1ULL << csize_shift) - 1;

    uint32_t nb_csectors =
        static_cast<uint32_t>(((l2_entry >> csize_shift) & csize_mask) + 1);
    uint64_t host_off = l2_entry & off_mask;
    // Match QEMU's qcow2_parse_compressed_l2_entry: subtract the
    // byte offset within the first 512-byte sector.
    uint64_t comp_len = static_cast<uint64_t>(nb_csectors) * 512
                        - (host_off & 511);

    uint64_t c_start = host_off >> cluster_bits_;
    uint64_t c_end = (host_off + comp_len - 1) >> cluster_bits_;
    for (uint64_t c = c_start; c <= c_end; c++) {
        FreeCluster(c << cluster_bits_);
    }
}

uint64_t* Qcow2DiskImage::EnsureL2Table(uint32_t l1_idx) {
    if (l1_idx >= l1_size_) return nullptr;

    uint64_t l1_entry = l1_table_[l1_idx];
    if (l1_entry != 0) {
        uint64_t l2_off = l1_entry & kOffsetMask;
        return GetL2Table(l2_off);
    }

    // Allocate new L2 table
    uint64_t new_l2_off = AllocateCluster();
    if (new_l2_off == 0) {
        LOG_ERROR("Qcow2: failed to allocate L2 table for l1_idx %u", l1_idx);
        return nullptr;
    }

    // Zero out the cluster so stale data isn't interpreted as L2 entries.
    // AllocateCluster only zeroes when extending the file; reused clusters
    // (freed by FreeCompressedCluster / RepairLeaks) may contain old data.
    std::vector<uint8_t> zeros(cluster_size_, 0);
    _fseeki64(file_, new_l2_off, SEEK_SET);
    if (fwrite(zeros.data(), 1, cluster_size_, file_) != cluster_size_) {
        LOG_ERROR("Qcow2: failed to zero L2 table at 0x%" PRIX64, new_l2_off);
        return nullptr;
    }

    // Update L1 entry (set COPIED bit)
    l1_table_[l1_idx] = new_l2_off | kCopiedBit;

    uint64_t be_entry = Be64(l1_table_[l1_idx]);
    _fseeki64(file_, l1_table_offset_ + l1_idx * sizeof(uint64_t), SEEK_SET);
    if (fwrite(&be_entry, sizeof(be_entry), 1, file_) != 1) {
        LOG_ERROR("Qcow2: failed to write L1 entry for l1_idx %u", l1_idx);
        return nullptr;
    }

    return GetL2Table(new_l2_off);
}

// ---------- public Read/Write ----------

bool Qcow2DiskImage::Read(uint64_t offset, void* buf, uint32_t len) {
    uint8_t* dst = static_cast<uint8_t*>(buf);

    while (len > 0) {
        uint64_t in_cluster_off = offset & (cluster_size_ - 1);
        uint32_t chunk = std::min(len,
            static_cast<uint32_t>(cluster_size_ - in_cluster_off));

        if (offset + chunk > virtual_size_) {
            LOG_ERROR("Qcow2: read past virtual disk end");
            return false;
        }

        bool compressed = false;
        uint64_t comp_host_off = 0;
        uint32_t comp_size = 0;
        uint64_t host_off = ResolveOffset(offset, &compressed,
                                           &comp_host_off, &comp_size);

        if (compressed) {
            if (!ReadCompressedCluster(comp_host_off, comp_size,
                                        in_cluster_off, dst, chunk)) {
                return false;
            }
        } else if (host_off == 0) {
            memset(dst, 0, chunk);
        } else {
            if (!ReadCluster(host_off, in_cluster_off, dst, chunk)) {
                return false;
            }
        }

        offset += chunk;
        dst += chunk;
        len -= chunk;
    }
    return true;
}

bool Qcow2DiskImage::Write(uint64_t offset, const void* buf, uint32_t len) {
    const uint8_t* src = static_cast<const uint8_t*>(buf);

    while (len > 0) {
        uint64_t in_cluster_off = offset & (cluster_size_ - 1);
        uint32_t chunk = std::min(len,
            static_cast<uint32_t>(cluster_size_ - in_cluster_off));

        if (offset + chunk > virtual_size_) {
            LOG_ERROR("Qcow2: write past virtual disk end");
            return false;
        }

        uint32_t l1_idx = static_cast<uint32_t>(
            offset / (static_cast<uint64_t>(l2_entries_) * cluster_size_));
        uint32_t l2_idx = static_cast<uint32_t>(
            (offset / cluster_size_) % l2_entries_);

        uint64_t* l2 = EnsureL2Table(l1_idx);
        if (!l2) return false;

        uint64_t l2_entry = l2[l2_idx];
        uint64_t data_off = 0;

        // Per QCOW2 spec: bit 63 (COPIED) = 1 means refcount == 1, safe to
        // write in-place.  Anything else (unallocated, compressed, shared
        // with refcount > 1, or zero-flagged) requires COW into a new cluster.
        bool need_cow = !(l2_entry & kCopiedBit) || (l2_entry & kCompressedBit) ||
                        (version_ >= 3 && (l2_entry & kZeroFlag));

        if (need_cow) {
            data_off = AllocateCluster();
            if (data_off == 0) {
                LOG_ERROR("Qcow2: failed to allocate data cluster");
                return false;
            }

            // COW: read old cluster data before we touch the L2 entry.
            if (chunk < cluster_size_ && l2_entry != 0) {
                std::vector<uint8_t> old_data(cluster_size_, 0);
                bool comp = false;
                uint64_t comp_off = 0;
                uint32_t comp_sz = 0;
                uint64_t old_host = ResolveOffset(
                    offset & ~(static_cast<uint64_t>(cluster_size_) - 1),
                    &comp, &comp_off, &comp_sz);

                if (comp) {
                    ReadCompressedCluster(comp_off, comp_sz, 0,
                                          old_data.data(), cluster_size_);
                } else if (old_host != 0) {
                    ReadCluster(old_host, 0, old_data.data(), cluster_size_);
                }

                WriteCluster(data_off, 0, old_data.data(), cluster_size_);
            }

            // Match QEMU handle_alloc order:
            //   1. Update L2 entry to point to the new cluster FIRST.
            //   2. Then free the old cluster(s).
            // If we crash between 1 and 2, the old cluster is leaked (harmless).
            // The reverse order (free then update L2) would leave L2 pointing
            // to a freed cluster on crash — potential data corruption.
            l2[l2_idx] = data_off | kCopiedBit;

            uint64_t l2_table_off = l1_table_[l1_idx] & kOffsetMask;
            auto it = l2_map_.find(l2_table_off);
            if (it != l2_map_.end()) {
                it->second->dirty = true;
            }

            // Now safe to free old cluster(s).
            if (l2_entry != 0) {
                if (l2_entry & kCompressedBit) {
                    FreeCompressedCluster(l2_entry);
                } else {
                    uint64_t old_off = l2_entry & kOffsetMask;
                    if (old_off != 0) {
                        FreeCluster(old_off);
                    }
                }
            }
        } else {
            data_off = l2_entry & kOffsetMask;
        }

        if (!WriteCluster(data_off, in_cluster_off, src, chunk)) {
            return false;
        }

        offset += chunk;
        src += chunk;
        len -= chunk;
    }
    return true;
}

bool Qcow2DiskImage::Discard(uint64_t offset, uint64_t len) {
    while (len > 0) {
        uint64_t in_cluster_off = offset & (cluster_size_ - 1);
        uint64_t chunk = std::min(len,
            static_cast<uint64_t>(cluster_size_) - in_cluster_off);

        // Only discard whole clusters -- partial cluster discard is a no-op
        if (in_cluster_off == 0 && chunk >= cluster_size_) {
            uint32_t l1_idx = static_cast<uint32_t>(
                offset / (static_cast<uint64_t>(l2_entries_) * cluster_size_));
            uint32_t l2_idx = static_cast<uint32_t>(
                (offset / cluster_size_) % l2_entries_);

            if (l1_idx < l1_size_) {
                uint64_t l1_entry = l1_table_[l1_idx];
                if (l1_entry != 0) {
                    uint64_t l2_table_off = l1_entry & kOffsetMask;
                    uint64_t* l2 = GetL2Table(l2_table_off);
                    if (l2 && l2[l2_idx] != 0) {
                        uint64_t l2_entry = l2[l2_idx];

                        // Update L2 first (crash-safe: leaked > corrupted).
                        l2[l2_idx] = 0;
                        auto it = l2_map_.find(l2_table_off);
                        if (it != l2_map_.end())
                            it->second->dirty = true;

                        // Then free old cluster(s).
                        if (l2_entry & kCompressedBit) {
                            FreeCompressedCluster(l2_entry);
                        } else {
                            uint64_t host_off = l2_entry & kOffsetMask;
                            if (host_off != 0) {
                                FreeCluster(host_off);
                            }
                        }
                    }
                }
            }
        }

        offset += chunk;
        len -= chunk;
    }
    return true;
}

bool Qcow2DiskImage::WriteZeros(uint64_t offset, uint64_t len) {
    while (len > 0) {
        uint64_t in_cluster_off = offset & (cluster_size_ - 1);
        uint64_t chunk = std::min(len,
            static_cast<uint64_t>(cluster_size_) - in_cluster_off);

        if (in_cluster_off == 0 && chunk >= cluster_size_) {
            // Whole cluster: discard (reads back as zeros)
            if (!Discard(offset, cluster_size_)) return false;
        } else {
            // Partial cluster: write actual zeros
            std::vector<uint8_t> zeros(static_cast<size_t>(chunk), 0);
            if (!Write(offset, zeros.data(), static_cast<uint32_t>(chunk))) {
                return false;
            }
        }

        offset += chunk;
        len -= chunk;
    }
    return true;
}

bool Qcow2DiskImage::Flush() {
    if (!file_) return false;

    for (auto& entry : l2_lru_) {
        if (entry.dirty) {
            std::vector<uint64_t> be_data(l2_entries_);
            for (uint32_t i = 0; i < l2_entries_; i++) {
                be_data[i] = Be64(entry.data[i]);
            }
            size_t bytes = l2_entries_ * sizeof(uint64_t);
            _fseeki64(file_, entry.l2_offset, SEEK_SET);
            if (fwrite(be_data.data(), 1, bytes, file_) != bytes) {
                LOG_ERROR("Qcow2: Flush: failed to write L2 table at 0x%" PRIX64,
                          entry.l2_offset);
            }
            entry.dirty = false;
        }
    }

    for (auto& entry : rfb_lru_) {
        if (entry.dirty) {
            std::vector<uint16_t> be_data(rfb_entries_);
            for (uint32_t i = 0; i < rfb_entries_; i++) {
                be_data[i] = Be16(entry.data[i]);
            }
            size_t bytes = rfb_entries_ * sizeof(uint16_t);
            _fseeki64(file_, entry.offset_in_file, SEEK_SET);
            if (fwrite(be_data.data(), 1, bytes, file_) != bytes) {
                LOG_ERROR("Qcow2: Flush: failed to write refcount block at 0x%" PRIX64,
                          entry.offset_in_file);
            }
            entry.dirty = false;
        }
    }

    // Flush refcount table if modified
    if (refcount_table_dirty_) {
        FlushRefcountTable();
    }

    fflush(file_);
#ifdef _WIN32
    _commit(_fileno(file_));
#else
    fsync(fileno(file_));
#endif
    return true;
}
