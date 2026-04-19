// Standalone unit tests for the Qcow2DiskImage implementation.
// Verifies: incompatible_features check, GrowRefcountTable, Write/Read
// correctness, dirty bit management, REFT_OFFSET_MASK, metadata overlap
// protection, compressed cluster COW/Free, and Discard.
//
// Tests 2 and 7 use qemu-img (via Docker) to create images that exercise
// GrowRefcountTable and FreeCompressedCluster.  Several tests cross-validate
// results with `qemu-img check`.

#include "core/disk/qcow2.h"
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

#ifdef _WIN32
#include <io.h>
#else
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

// ── byte-swap helpers (portable; MSVC has no __builtin_bswap*) ────────
static uint16_t be16(uint16_t v) {
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}
static uint32_t be32(uint32_t v) {
    v = ((v & 0x0000FFFFu) << 16) | ((v & 0xFFFF0000u) >> 16);
    v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    return v;
}
static uint64_t be64(uint64_t v) {
    v = ((v & 0x00000000FFFFFFFFull) << 32) | ((v & 0xFFFFFFFF00000000ull) >> 32);
    v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v & 0xFFFF0000FFFF0000ull) >> 16);
    v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v & 0xFF00FF00FF00FF00ull) >> 8);
    return v;
}

// ── test infrastructure ──────────────────────────────────────────────
static int g_pass = 0, g_fail = 0;

#define TEST_ASSERT(cond, msg)                                          \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "  ASSERT FAILED: %s  (%s:%d)\n",          \
                    msg, __FILE__, __LINE__);                           \
            return false;                                               \
        }                                                               \
    } while (0)

static void RunTest(const char* name, std::function<bool()> fn) {
    fprintf(stdout, "--- %s ---\n", name);
    bool ok = fn();
    if (ok) { g_pass++; fprintf(stdout, "  PASS\n"); }
    else    { g_fail++; fprintf(stdout, "  FAIL\n"); }
}

// ── Docker / qemu-img helpers ────────────────────────────────────────

static const char* kDockerPrefix =
    "docker run --rm --entrypoint \"\" -v /tmp:/tmp tenbox-builder qemu-img ";

static bool QemuImg(const std::string& args) {
    std::string cmd = kDockerPrefix + args + " 2>&1";
    return system(cmd.c_str()) == 0;
}

// Run `qemu-img check` and return true if the image is clean.
// Prints the check output regardless.
static bool QemuImgCheck(const std::string& path) {
    std::string cmd = std::string(kDockerPrefix) + "check " + path + " 2>&1";
#ifdef _WIN32
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return false;
    std::string output;
    char buf[512];
    while (fgets(buf, sizeof(buf), p))
        output += buf;
#ifdef _WIN32
    int ret = _pclose(p);
#else
    int ret = pclose(p);
#endif
    bool ok = (ret == 0) && (output.find("No errors") != std::string::npos);
    fprintf(stdout, "  qemu-img check: %s", output.c_str());
    if (!ok)
        fprintf(stderr, "  qemu-img check FAILED (exit %d)\n", ret);
    return ok;
}

// ── helper: create a minimal valid qcow2 v3 image ───────────────────
// Layout (cluster_size = 65536 = 0x10000):
//   Cluster 0: header
//   Cluster 1: L1 table
//   Cluster 2: refcount table
//   Cluster 3: refcount block 0 (covers clusters 0..32767)
// Total file size: 4 * 65536 = 262144

static const uint32_t kClusterBits = 16;
static const uint32_t kClusterSize = 1u << kClusterBits;

struct CreateOpts {
    uint64_t virtual_size = 64 * 1024 * 1024;
    uint64_t incompat_features = 0;
};

static bool CreateMinimalQcow2(const std::string& path, const CreateOpts& opts) {
    FILE* f = fopen(path.c_str(), "w+b");
    if (!f) return false;

    const uint32_t l2_entries = kClusterSize / 8;
    uint32_t l1_size = static_cast<uint32_t>(
        (opts.virtual_size + static_cast<uint64_t>(l2_entries) * kClusterSize - 1)
        / (static_cast<uint64_t>(l2_entries) * kClusterSize));

    const uint64_t l1_off   = 1ULL * kClusterSize;
    const uint64_t rft_off  = 2ULL * kClusterSize;
    const uint64_t rfb0_off = 3ULL * kClusterSize;

    Qcow2Header hdr{};
    hdr.magic                  = be32(0x514649FB);
    hdr.version                = be32(3);
    hdr.cluster_bits           = be32(kClusterBits);
    hdr.size                   = be64(opts.virtual_size);
    hdr.l1_size                = be32(l1_size);
    hdr.l1_table_offset        = be64(l1_off);
    hdr.refcount_table_offset  = be64(rft_off);
    hdr.refcount_table_clusters = be32(1);
    hdr.incompatible_features  = be64(opts.incompat_features);
    hdr.refcount_order         = be32(4);
    hdr.header_length          = be32(104);

    fseek(f, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, f);

    // Zero-fill clusters 1-3
    std::vector<uint8_t> zeros(kClusterSize, 0);
    fseek(f, kClusterSize, SEEK_SET);
    for (int i = 0; i < 3; i++)
        fwrite(zeros.data(), 1, kClusterSize, f);

    // Refcount table entry 0 → refcount block at cluster 3
    uint64_t rft_entry0 = be64(rfb0_off);
    fseek(f, static_cast<long>(rft_off), SEEK_SET);
    fwrite(&rft_entry0, sizeof(rft_entry0), 1, f);

    // Refcount block: clusters 0-3 = refcount 1
    for (uint32_t i = 0; i < 4; i++) {
        uint16_t one = be16(1);
        fseek(f, static_cast<long>(rfb0_off + i * sizeof(uint16_t)), SEEK_SET);
        fwrite(&one, sizeof(one), 1, f);
    }

    fflush(f);
    fclose(f);
    return true;
}

// ── file helpers ─────────────────────────────────────────────────────

static uint64_t ReadBe64(const std::string& path, long offset) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    uint64_t val = 0;
    fseek(f, offset, SEEK_SET);
    size_t n = fread(&val, sizeof(val), 1, f);
    (void)n;
    fclose(f);
    return be64(val);
}

static uint32_t ReadBe32(const std::string& path, long offset) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    uint32_t val = 0;
    fseek(f, offset, SEEK_SET);
    size_t n = fread(&val, sizeof(val), 1, f);
    (void)n;
    fclose(f);
    return be32(val);
}

static void WriteBe64(const std::string& path, long offset, uint64_t val) {
    FILE* f = fopen(path.c_str(), "r+b");
    if (!f) return;
    uint64_t be = be64(val);
    fseek(f, offset, SEEK_SET);
    fwrite(&be, sizeof(be), 1, f);
    fflush(f);
    fclose(f);
}

// ── Test 1: incompatible_features rejects unknown bits ───────────────
static bool TestIncompatFeatures() {
    std::string path = "/tmp/test_qcow2_incompat.qcow2";

    // bit 4 (Extended L2) → reject
    {
        CreateOpts opts;
        opts.incompat_features = (1ULL << 4);
        TEST_ASSERT(CreateMinimalQcow2(path, opts), "create failed");
        Qcow2DiskImage img;
        TEST_ASSERT(!img.Open(path), "Open should fail with incompat bit 4");
    }

    // bit 1 (corrupt) → reject
    {
        CreateOpts opts;
        opts.incompat_features = (1ULL << 1);
        TEST_ASSERT(CreateMinimalQcow2(path, opts), "create failed");
        Qcow2DiskImage img;
        TEST_ASSERT(!img.Open(path), "Open should fail with corrupt bit");
    }

    // bit 0 only (dirty) → accept
    {
        CreateOpts opts;
        opts.incompat_features = (1ULL << 0);
        TEST_ASSERT(CreateMinimalQcow2(path, opts), "create failed");
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Open should succeed with dirty bit");
    }

    // no bits → accept
    {
        CreateOpts opts;
        TEST_ASSERT(CreateMinimalQcow2(path, opts), "create failed");
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Open should succeed with no incompat");
    }

    std::remove(path.c_str());
    return true;
}

// ── Test 2: GrowRefcountTable correctness ────────────────────────────
// Uses cluster_size=512 via qemu-img so the refcount table has only
// 64 entries covering 16384 clusters (8 MB).  Writing >8 MB of physical
// data forces GrowRefcountTable to execute.
static bool TestGrowRefcountTable() {
    std::string path = "/tmp/test_qcow2_grow_rft.qcow2";
    std::remove(path.c_str());

    // Create image with cluster_size=512 (refcount table covers only ~8 MB)
    TEST_ASSERT(QemuImg("create -f qcow2 -o cluster_size=512 " + path + " 16M"),
                "qemu-img create failed");

    // Record initial refcount table offset & cluster count
    uint64_t orig_rft_off = ReadBe64(path, 48);
    uint32_t orig_rft_clusters = ReadBe32(path, 56);
    fprintf(stdout, "  initial rft_off=0x%" PRIX64 " rft_clusters=%u\n",
            orig_rft_off, orig_rft_clusters);

    // Phase 1: write enough data to trigger GrowRefcountTable
    // With 512-byte clusters, 16384 clusters = 8 MB.
    // Write 10 MB to be safe (data + L2 tables > 16384 clusters).
    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Open failed");

        std::vector<uint8_t> wbuf(65536);
        uint64_t total_written = 0;
        const uint64_t target = 10ULL * 1024 * 1024;
        while (total_written < target) {
            uint32_t chunk = static_cast<uint32_t>(
                std::min<uint64_t>(sizeof(wbuf[0]) * wbuf.size(),
                                   target - total_written));
            memset(wbuf.data(), static_cast<int>((total_written / 512) & 0xFF), chunk);
            TEST_ASSERT(img.Write(total_written, wbuf.data(), chunk),
                        "Write failed during growth");
            total_written += chunk;
        }
        TEST_ASSERT(img.Flush(), "Flush failed");
    }

    // Verify refcount table actually grew
    uint64_t new_rft_off = ReadBe64(path, 48);
    uint32_t new_rft_clusters = ReadBe32(path, 56);
    fprintf(stdout, "  after growth rft_off=0x%" PRIX64 " rft_clusters=%u\n",
            new_rft_off, new_rft_clusters);
    TEST_ASSERT(new_rft_off != orig_rft_off || new_rft_clusters > orig_rft_clusters,
                "refcount table did not grow");

    // Phase 2: reopen, verify data + integrity
    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Reopen failed");

        // Spot-check a few 512-byte chunks
        std::vector<uint8_t> rbuf(512);
        for (uint64_t off = 0; off < 10ULL * 1024 * 1024; off += 1024 * 1024) {
            TEST_ASSERT(img.Read(off, rbuf.data(), 512), "Read failed");
            uint8_t expected = static_cast<uint8_t>((off / 512) & 0xFF);
            TEST_ASSERT(rbuf[0] == expected, "data mismatch after growth");
        }

        int issues = img.RepairLeaks(false);
        TEST_ASSERT(issues == 0, "RepairLeaks found issues after growth");
    }

    // Phase 3: write more data to verify new table works correctly
    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Reopen for phase 3 failed");

        std::vector<uint8_t> wbuf(65536, 0xAB);
        TEST_ASSERT(img.Write(10ULL * 1024 * 1024, wbuf.data(), 65536),
                    "post-grow write failed");

        std::vector<uint8_t> rbuf(65536);
        TEST_ASSERT(img.Read(10ULL * 1024 * 1024, rbuf.data(), 65536),
                    "post-grow read failed");
        TEST_ASSERT(rbuf[0] == 0xAB, "post-grow data mismatch");

        int issues = img.RepairLeaks(false);
        TEST_ASSERT(issues == 0, "RepairLeaks found issues in phase 3");
    }

    // Cross-validate with qemu-img check
    TEST_ASSERT(QemuImgCheck(path), "qemu-img check failed after growth test");

    std::remove(path.c_str());
    return true;
}

// ── Test 3: Write/Read data consistency + refcount correctness ───────
static bool TestWriteReadConsistency() {
    std::string path = "/tmp/test_qcow2_wrcons.qcow2";
    CreateOpts opts;
    opts.virtual_size = 16 * 1024 * 1024;
    TEST_ASSERT(CreateMinimalQcow2(path, opts), "create failed");

    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Open failed");

        // Full cluster write
        std::vector<uint8_t> full(kClusterSize, 0xAA);
        TEST_ASSERT(img.Write(0, full.data(), kClusterSize), "full write failed");

        // Partial write at offset 100 within cluster 1
        std::vector<uint8_t> partial(200, 0xBB);
        TEST_ASSERT(img.Write(kClusterSize + 100, partial.data(), 200),
                    "partial write failed");

        // Cross-cluster write straddling boundary
        uint64_t cross_off = 2ULL * kClusterSize - 50;
        std::vector<uint8_t> cross(100, 0xCC);
        TEST_ASSERT(img.Write(cross_off, cross.data(), 100),
                    "cross-cluster write failed");
        img.Flush();
    }

    // Reopen and verify
    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Reopen failed");

        std::vector<uint8_t> buf(kClusterSize);
        TEST_ASSERT(img.Read(0, buf.data(), kClusterSize), "read full failed");
        for (uint32_t i = 0; i < kClusterSize; i++)
            TEST_ASSERT(buf[i] == 0xAA, "full cluster data mismatch");

        std::vector<uint8_t> pbuf(200);
        TEST_ASSERT(img.Read(kClusterSize + 100, pbuf.data(), 200),
                    "read partial failed");
        for (int i = 0; i < 200; i++)
            TEST_ASSERT(pbuf[i] == 0xBB, "partial data mismatch");

        uint8_t before = 0xFF;
        TEST_ASSERT(img.Read(kClusterSize + 99, &before, 1), "read before partial");
        TEST_ASSERT(before == 0x00, "byte before partial should be zero");

        uint64_t cross_off = 2ULL * kClusterSize - 50;
        std::vector<uint8_t> cbuf(100);
        TEST_ASSERT(img.Read(cross_off, cbuf.data(), 100), "read cross failed");
        for (int i = 0; i < 100; i++)
            TEST_ASSERT(cbuf[i] == 0xCC, "cross-cluster data mismatch");

        int issues = img.RepairLeaks(false);
        TEST_ASSERT(issues == 0, "RepairLeaks found issues");
    }

    TEST_ASSERT(QemuImgCheck(path), "qemu-img cross-validation failed");

    std::remove(path.c_str());
    return true;
}

// ── Test 4: Dirty bit management ─────────────────────────────────────
static bool TestDirtyBit() {
    std::string path = "/tmp/test_qcow2_dirty.qcow2";
    CreateOpts opts;
    TEST_ASSERT(CreateMinimalQcow2(path, opts), "create failed");

    uint64_t incompat_before = ReadBe64(path, 72);
    TEST_ASSERT((incompat_before & 1) == 0, "dirty bit should be clear initially");

    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Open failed");
        uint64_t incompat_during = ReadBe64(path, 72);
        TEST_ASSERT((incompat_during & 1) == 1,
                    "dirty bit should be set while open");
    }

    uint64_t incompat_after = ReadBe64(path, 72);
    TEST_ASSERT((incompat_after & 1) == 0,
                "dirty bit should be clear after clean close");

    std::remove(path.c_str());
    return true;
}

// ── Test 5: REFT_OFFSET_MASK — reserved bits in refcount table ───────
static bool TestReftOffsetMask() {
    std::string path = "/tmp/test_qcow2_reftmask.qcow2";
    CreateOpts opts;
    opts.virtual_size = 8 * 1024 * 1024;
    TEST_ASSERT(CreateMinimalQcow2(path, opts), "create failed");

    const long rft_off = 2 * kClusterSize;
    uint64_t entry0 = ReadBe64(path, rft_off);
    TEST_ASSERT(entry0 == 3ULL * kClusterSize,
                "initial rft entry should point to cluster 3");

    // Set reserved bits 0-8 to garbage
    WriteBe64(path, rft_off, entry0 | 0x1FF);

    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Open failed with corrupted reserved bits");

        std::vector<uint8_t> data(4096, 0x42);
        TEST_ASSERT(img.Write(0, data.data(), 4096), "write failed");

        std::vector<uint8_t> rbuf(4096);
        TEST_ASSERT(img.Read(0, rbuf.data(), 4096), "read failed");
        TEST_ASSERT(rbuf[0] == 0x42, "readback mismatch");

        int issues = img.RepairLeaks(false);
        TEST_ASSERT(issues == 0, "RepairLeaks found issues");
    }

    TEST_ASSERT(QemuImgCheck(path), "qemu-img cross-validation failed");

    std::remove(path.c_str());
    return true;
}

// ── Test 6: Metadata overlap protection ──────────────────────────────
static bool TestMetadataOverlap() {
    std::string path = "/tmp/test_qcow2_overlap.qcow2";
    CreateOpts opts;
    opts.virtual_size = 16 * 1024 * 1024;
    TEST_ASSERT(CreateMinimalQcow2(path, opts), "create failed");

    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Open failed");

        std::vector<uint8_t> data(kClusterSize);
        for (int i = 0; i < 128; i++) {
            memset(data.data(), i & 0xFF, kClusterSize);
            TEST_ASSERT(img.Write(static_cast<uint64_t>(i) * kClusterSize,
                                  data.data(), kClusterSize),
                        "write failed");
        }
        img.Flush();

        for (int i = 0; i < 128; i++) {
            TEST_ASSERT(img.Read(static_cast<uint64_t>(i) * kClusterSize,
                                 data.data(), kClusterSize),
                        "read failed");
            TEST_ASSERT(data[0] == static_cast<uint8_t>(i & 0xFF),
                        "data mismatch after heavy write");
        }

        int issues = img.RepairLeaks(false);
        TEST_ASSERT(issues == 0, "RepairLeaks found issues");
    }

    TEST_ASSERT(QemuImgCheck(path), "qemu-img cross-validation failed");

    std::remove(path.c_str());
    return true;
}

// ── Test 7: Overwrite compressed clusters (FreeCompressedCluster) ─────
// Uses qemu-img to create a qcow2 with compressed clusters, then
// overwrites them with our code (triggering FreeCompressedCluster).
static bool TestCompressedClusterOverwrite() {
    std::string raw_path = "/tmp/test_qcow2_comp_raw.img";
    std::string path     = "/tmp/test_qcow2_comp.qcow2";
    std::remove(raw_path.c_str());
    std::remove(path.c_str());

    // Create a 4 MB raw image with a known pattern:
    // each 64 KB cluster filled with (cluster_index & 0xFF)
    {
        FILE* f = fopen(raw_path.c_str(), "wb");
        TEST_ASSERT(f != nullptr, "create raw failed");
        std::vector<uint8_t> buf(kClusterSize);
        for (int i = 0; i < 64; i++) {
            memset(buf.data(), i & 0xFF, kClusterSize);
            fwrite(buf.data(), 1, kClusterSize, f);
        }
        fclose(f);
    }

    // Convert to compressed qcow2 via qemu-img
    TEST_ASSERT(QemuImg("convert -c -f raw -O qcow2 " + raw_path + " " + path),
                "qemu-img convert -c failed");
    std::remove(raw_path.c_str());

    // Verify qemu-img reports compressed clusters
    fprintf(stdout, "  Initial image state:\n");
    TEST_ASSERT(QemuImgCheck(path), "qemu-img check on compressed image failed");

    // Phase 1: Open with our code, read and verify compressed data
    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Open compressed failed");

        std::vector<uint8_t> rbuf(kClusterSize);
        for (int i = 0; i < 64; i++) {
            TEST_ASSERT(img.Read(static_cast<uint64_t>(i) * kClusterSize,
                                 rbuf.data(), kClusterSize),
                        "read compressed cluster failed");
            uint8_t expected = static_cast<uint8_t>(i & 0xFF);
            if (rbuf[0] != expected) {
                fprintf(stderr, "  compressed data mismatch at cluster %d: "
                        "got 0x%02X expected 0x%02X\n", i, rbuf[0], expected);
                return false;
            }
        }

        // Phase 2: Overwrite first 16 clusters (triggers COW +
        // FreeCompressedCluster for each compressed cluster)
        std::vector<uint8_t> wbuf(kClusterSize, 0xEE);
        for (int i = 0; i < 16; i++) {
            TEST_ASSERT(img.Write(static_cast<uint64_t>(i) * kClusterSize,
                                  wbuf.data(), kClusterSize),
                        "overwrite compressed cluster failed");
        }

        // Phase 3: Discard clusters 16-31 (triggers FreeCompressedCluster
        // via Discard path)
        for (int i = 16; i < 32; i++) {
            TEST_ASSERT(img.Discard(static_cast<uint64_t>(i) * kClusterSize,
                                    kClusterSize),
                        "discard compressed cluster failed");
        }

        img.Flush();

        // Verify overwritten data
        for (int i = 0; i < 16; i++) {
            TEST_ASSERT(img.Read(static_cast<uint64_t>(i) * kClusterSize,
                                 rbuf.data(), kClusterSize),
                        "read overwritten failed");
            TEST_ASSERT(rbuf[0] == 0xEE, "overwritten data mismatch");
        }

        // Discarded clusters should read as zeros
        for (int i = 16; i < 32; i++) {
            TEST_ASSERT(img.Read(static_cast<uint64_t>(i) * kClusterSize,
                                 rbuf.data(), kClusterSize),
                        "read discarded compressed failed");
            TEST_ASSERT(rbuf[0] == 0x00, "discarded cluster not zero");
        }

        // Remaining compressed clusters should be intact
        for (int i = 32; i < 64; i++) {
            TEST_ASSERT(img.Read(static_cast<uint64_t>(i) * kClusterSize,
                                 rbuf.data(), kClusterSize),
                        "read remaining compressed failed");
            uint8_t expected = static_cast<uint8_t>(i & 0xFF);
            TEST_ASSERT(rbuf[0] == expected, "remaining compressed data changed");
        }

        int issues = img.RepairLeaks(false);
        TEST_ASSERT(issues == 0, "RepairLeaks found issues after compressed overwrite");
    }

    // Cross-validate final image
    fprintf(stdout, "  After overwrite/discard:\n");
    TEST_ASSERT(QemuImgCheck(path), "qemu-img check failed after compressed test");

    std::remove(path.c_str());
    return true;
}

// ── Test 8: Discard whole cluster ────────────────────────────────────
static bool TestDiscard() {
    std::string path = "/tmp/test_qcow2_discard.qcow2";
    CreateOpts opts;
    opts.virtual_size = 8 * 1024 * 1024;
    TEST_ASSERT(CreateMinimalQcow2(path, opts), "create failed");

    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Open failed");

        std::vector<uint8_t> data(kClusterSize, 0xDD);
        for (int i = 0; i < 3; i++)
            TEST_ASSERT(img.Write(static_cast<uint64_t>(i) * kClusterSize,
                                  data.data(), kClusterSize),
                        "write failed");

        TEST_ASSERT(img.Discard(kClusterSize, kClusterSize), "discard failed");

        std::vector<uint8_t> rbuf(kClusterSize, 0xFF);
        TEST_ASSERT(img.Read(kClusterSize, rbuf.data(), kClusterSize),
                    "read discarded failed");
        for (uint32_t i = 0; i < kClusterSize; i++)
            TEST_ASSERT(rbuf[i] == 0x00, "discarded cluster should be zeros");

        TEST_ASSERT(img.Read(0, rbuf.data(), kClusterSize), "read cluster 0");
        TEST_ASSERT(rbuf[0] == 0xDD, "cluster 0 data lost");
        TEST_ASSERT(img.Read(2ULL * kClusterSize, rbuf.data(), kClusterSize),
                    "read cluster 2");
        TEST_ASSERT(rbuf[0] == 0xDD, "cluster 2 data lost");

        int issues = img.RepairLeaks(false);
        TEST_ASSERT(issues == 0, "RepairLeaks found issues");
    }

    // Reopen to verify persistence
    {
        Qcow2DiskImage img;
        TEST_ASSERT(img.Open(path), "Reopen failed");

        std::vector<uint8_t> rbuf(kClusterSize);
        TEST_ASSERT(img.Read(kClusterSize, rbuf.data(), kClusterSize),
                    "read discarded after reopen");
        TEST_ASSERT(rbuf[0] == 0x00, "discarded cluster not zero after reopen");

        TEST_ASSERT(img.Read(0, rbuf.data(), kClusterSize), "read c0 reopen");
        TEST_ASSERT(rbuf[0] == 0xDD, "c0 lost after reopen");

        int issues = img.RepairLeaks(false);
        TEST_ASSERT(issues == 0, "RepairLeaks found issues on reopen");
    }

    TEST_ASSERT(QemuImgCheck(path), "qemu-img cross-validation failed");

    std::remove(path.c_str());
    return true;
}

// ── main ─────────────────────────────────────────────────────────────
int main() {
    fprintf(stdout, "=== QCOW2 Unit Tests ===\n\n");

    RunTest("Test 1: incompatible_features",        TestIncompatFeatures);
    RunTest("Test 2: GrowRefcountTable",            TestGrowRefcountTable);
    RunTest("Test 3: Write/Read consistency",        TestWriteReadConsistency);
    RunTest("Test 4: Dirty bit management",          TestDirtyBit);
    RunTest("Test 5: REFT_OFFSET_MASK",              TestReftOffsetMask);
    RunTest("Test 6: Metadata overlap",              TestMetadataOverlap);
    RunTest("Test 7: Compressed cluster overwrite",  TestCompressedClusterOverwrite);
    RunTest("Test 8: Discard",                       TestDiscard);

    fprintf(stdout, "\n=== Results: %d passed, %d failed ===\n",
            g_pass, g_fail);
    return g_fail;
}
