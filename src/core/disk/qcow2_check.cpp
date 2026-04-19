#include "core/disk/qcow2.h"
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

int Qcow2DiskImage::RepairLeaks(bool fix) {
    if (!file_) return -1;

    Flush();

    uint64_t total_clusters = file_end_ >> cluster_bits_;
    if (total_clusters == 0) return 0;

    // Phase 1: build computed reference counts by scanning all metadata
    std::vector<uint16_t> computed(total_clusters, 0);

    auto add_ref = [&](uint64_t host_offset) {
        uint64_t idx = host_offset >> cluster_bits_;
        if (idx < total_clusters && computed[idx] < UINT16_MAX)
            computed[idx]++;
    };

    // Header cluster
    add_ref(0);

    // L1 table clusters
    uint64_t l1_byte_len = static_cast<uint64_t>(l1_size_) * sizeof(uint64_t);
    uint64_t l1_nclusters = (l1_byte_len + cluster_size_ - 1) / cluster_size_;
    for (uint64_t i = 0; i < l1_nclusters; i++)
        add_ref(l1_table_offset_ + i * cluster_size_);

    // Refcount table clusters
    for (uint32_t i = 0; i < refcount_table_clusters_; i++)
        add_ref(refcount_table_offset_ + static_cast<uint64_t>(i) * cluster_size_);

    // Refcount block clusters (each non-zero entry in the refcount table)
    for (size_t i = 0; i < refcount_table_.size(); i++) {
        if (refcount_table_[i] != 0)
            add_ref(refcount_table_[i]);
    }

    // Walk L1 -> L2 -> data clusters, collecting guest-level statistics
    uint64_t allocated_clusters = 0;
    uint64_t compressed_clusters = 0;
    uint64_t fragmented_clusters = 0;
    for (uint32_t l1_idx = 0; l1_idx < l1_size_; l1_idx++) {
        uint64_t l1_entry = l1_table_[l1_idx];
        if (l1_entry == 0) continue;

        uint64_t l2_offset = l1_entry & kOffsetMask;
        if (l2_offset == 0) continue;

        add_ref(l2_offset);

        // Read L2 table directly from disk (bypass cache for consistency)
        std::vector<uint64_t> l2_raw(l2_entries_);
        uint64_t prev_host_end = 0;
        _fseeki64(file_, l2_offset, SEEK_SET);
        size_t l2_bytes = l2_entries_ * sizeof(uint64_t);
        if (fread(l2_raw.data(), 1, l2_bytes, file_) != l2_bytes) {
            LOG_ERROR("Qcow2: Check: failed to read L2 table at 0x%" PRIX64, l2_offset);
            continue;
        }

        for (uint32_t l2_idx = 0; l2_idx < l2_entries_; l2_idx++) {
            uint64_t l2_entry = Be64(l2_raw[l2_idx]);
            if (l2_entry == 0) continue;

            if (l2_entry & kCompressedBit) {
                allocated_clusters++;
                compressed_clusters++;
                uint32_t csize_shift = 62 - (cluster_bits_ - 8);
                uint64_t csize_mask = (1ULL << (cluster_bits_ - 8)) - 1;
                uint64_t off_mask = (1ULL << csize_shift) - 1;
                uint32_t nb_csectors =
                    static_cast<uint32_t>(((l2_entry >> csize_shift) & csize_mask) + 1);
                uint64_t host_off = l2_entry & off_mask;
                uint64_t comp_len = static_cast<uint64_t>(nb_csectors) * 512
                                    - (host_off & 511);
                uint64_t c_start = host_off >> cluster_bits_;
                uint64_t c_end = (host_off + comp_len - 1) >> cluster_bits_;
                for (uint64_t c = c_start; c <= c_end && c < total_clusters; c++) {
                    if (computed[c] < UINT16_MAX)
                        computed[c]++;
                }
                // Compressed clusters are always fragmented; don't touch
                // prev_host_end so the next normal cluster keeps its chain.
                fragmented_clusters++;
            } else {
                uint64_t host_off = l2_entry & kOffsetMask;
                if (host_off != 0) {
                    allocated_clusters++;
                    add_ref(host_off);
                    if (prev_host_end != 0 && host_off != prev_host_end)
                        fragmented_clusters++;
                    prev_host_end = host_off + cluster_size_;
                }
            }
        }
    }

    // Phase 2: read stored refcounts from disk and compare
    std::vector<uint16_t> stored(total_clusters, 0);

    for (size_t rft_idx = 0; rft_idx < refcount_table_.size(); rft_idx++) {
        uint64_t block_offset = refcount_table_[rft_idx];
        if (block_offset == 0) continue;

        std::vector<uint16_t> block(rfb_entries_);
        _fseeki64(file_, block_offset, SEEK_SET);
        size_t bytes = rfb_entries_ * sizeof(uint16_t);
        if (fread(block.data(), 1, bytes, file_) != bytes)
            continue;

        for (uint32_t i = 0; i < rfb_entries_; i++) {
            uint64_t cidx = rft_idx * rfb_entries_ + i;
            if (cidx >= total_clusters) break;
            stored[cidx] = Be16(block[i]);
        }
    }

    int leaked = 0;
    int errors = 0;

    for (uint64_t idx = 0; idx < total_clusters; idx++) {
        if (stored[idx] > computed[idx]) {
            LOG_INFO("Leaked cluster %" PRIu64 " refcount=%u reference=%u",
                     idx, stored[idx], computed[idx]);
            leaked++;
        } else if (stored[idx] < computed[idx]) {
            LOG_ERROR("ERROR cluster %" PRIu64 " refcount=%u reference=%u",
                      idx, stored[idx], computed[idx]);
            errors++;
        }
    }

    if (errors > 0) {
        LOG_ERROR("%d errors were found on the image.", errors);
        LOG_ERROR("Data may be corrupted, or further writes to the image may corrupt it.");
    }
    if (leaked > 0) {
        LOG_INFO("%d leaked clusters were found on the image.", leaked);
        LOG_INFO("This means waste of disk space, but no harm to data.");
    }
    if (errors == 0 && leaked == 0) {
        LOG_INFO("No errors or leaked clusters found.");
    }

    uint64_t total_guest_clusters = virtual_size_ / cluster_size_;
    LOG_INFO("%" PRIu64 "/%" PRIu64 " = %.2f%% allocated, %.2f%% fragmented, %.2f%% compressed clusters",
             allocated_clusters,
             total_guest_clusters,
             total_guest_clusters > 0
                 ? 100.0 * allocated_clusters / total_guest_clusters : 0.0,
             allocated_clusters > 0
                 ? 100.0 * fragmented_clusters / allocated_clusters : 0.0,
             allocated_clusters > 0
                 ? 100.0 * compressed_clusters / allocated_clusters : 0.0);
    LOG_INFO("Image end offset: %" PRIu64, file_end_);

    // Phase 3: fix leaked clusters only (stored > computed).
    // Errors (stored < computed) indicate possible corruption — never
    // auto-fix those; leave it to the user / qemu-img check -r.
    if (fix && leaked > 0) {
        int fixed = 0;

        rfb_lru_.clear();
        rfb_map_.clear();

        for (size_t rft_idx = 0; rft_idx < refcount_table_.size(); rft_idx++) {
            uint64_t block_offset = refcount_table_[rft_idx];
            if (block_offset == 0) continue;

            std::vector<uint16_t> block(rfb_entries_);
            _fseeki64(file_, block_offset, SEEK_SET);
            size_t bytes = rfb_entries_ * sizeof(uint16_t);
            if (fread(block.data(), 1, bytes, file_) != bytes)
                continue;

            bool dirty = false;
            for (uint32_t i = 0; i < rfb_entries_; i++) {
                uint64_t cidx = rft_idx * rfb_entries_ + i;
                if (cidx >= total_clusters) break;

                uint16_t cur = Be16(block[i]);
                uint16_t ref = computed[cidx];
                if (cur > ref) {
                    block[i] = Be16(ref);
                    dirty = true;
                    fixed++;
                }
            }

            if (dirty) {
                _fseeki64(file_, block_offset, SEEK_SET);
                if (fwrite(block.data(), 1, bytes, file_) != bytes) {
                    LOG_ERROR("Qcow2: RepairLeaks: failed to write refcount block "
                              "at 0x%" PRIX64, block_offset);
                }
            }
        }

        fflush(file_);
        LOG_INFO("Fixed %d leaked clusters.", fixed);
    }

    return leaked + errors;
}
