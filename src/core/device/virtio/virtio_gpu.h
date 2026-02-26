#pragma once

#include "common/ports.h"
#include "core/device/virtio/virtio_mmio.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

// Virtio GPU command types (spec 5.7.6)
constexpr uint32_t VIRTIO_GPU_CMD_GET_DISPLAY_INFO      = 0x0100;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    = 0x0101;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_UNREF        = 0x0102;
constexpr uint32_t VIRTIO_GPU_CMD_SET_SCANOUT           = 0x0103;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_FLUSH        = 0x0104;
constexpr uint32_t VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   = 0x0105;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;
constexpr uint32_t VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107;

// Response types
constexpr uint32_t VIRTIO_GPU_RESP_OK_NODATA            = 0x1100;
constexpr uint32_t VIRTIO_GPU_RESP_OK_DISPLAY_INFO      = 0x1101;
constexpr uint32_t VIRTIO_GPU_RESP_ERR_UNSPEC           = 0x1200;
constexpr uint32_t VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID = 0x1202;
constexpr uint32_t VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER  = 0x1203;

// Pixel formats
constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1;
constexpr uint32_t VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2;
constexpr uint32_t VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3;
constexpr uint32_t VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4;
constexpr uint32_t VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67;
constexpr uint32_t VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68;
constexpr uint32_t VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM = 121;
constexpr uint32_t VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM = 134;

// Feature bits
constexpr uint64_t VIRTIO_GPU_F_EDID = 1ULL << 1;

#pragma pack(push, 1)
struct VirtioGpuCtrlHdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

struct VirtioGpuRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct VirtioGpuDisplayOne {
    VirtioGpuRect r;
    uint32_t enabled;
    uint32_t flags;
};

struct VirtioGpuRespDisplayInfo {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuDisplayOne pmodes[16];
};

struct VirtioGpuResourceCreate2d {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct VirtioGpuResourceUnref {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct VirtioGpuSetScanout {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuRect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct VirtioGpuResourceFlush {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuRect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct VirtioGpuTransferToHost2d {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuRect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct VirtioGpuResourceAttachBacking {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
};

struct VirtioGpuMemEntry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct VirtioGpuResourceDetachBacking {
    VirtioGpuCtrlHdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct VirtioGpuConfig {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
};
#pragma pack(pop)

static_assert(sizeof(VirtioGpuCtrlHdr) == 24);

class VirtioGpuDevice : public VirtioDeviceOps {
public:
    using FrameCallback = std::function<void(const DisplayFrame&)>;

    VirtioGpuDevice(uint32_t width, uint32_t height);
    ~VirtioGpuDevice() override = default;

    void SetMmioDevice(VirtioMmioDevice* mmio) { mmio_ = mmio; }
    void SetMemMap(const GuestMemMap& mem) { mem_ = mem; }
    void SetFrameCallback(FrameCallback cb) { frame_callback_ = std::move(cb); }

    uint32_t GetDeviceId() const override { return 16; }
    uint64_t GetDeviceFeatures() const override;
    uint32_t GetNumQueues() const override { return 2; }
    uint32_t GetQueueMaxSize(uint32_t queue_idx) const override { return 256; }
    void OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) override;
    void ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) override;
    void WriteConfig(uint32_t offset, uint8_t size, uint32_t value) override;
    void OnStatusChange(uint32_t new_status) override;

private:
    struct GpuResource {
        uint32_t id = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;
        std::vector<uint8_t> host_pixels;
        struct BackingPage {
            uint64_t gpa;
            uint32_t length;
        };
        std::vector<BackingPage> backing;
    };

    void ProcessControlQueue(VirtQueue& vq);
    void HandleCommand(const uint8_t* req_buf, uint32_t req_len,
                       uint8_t* resp_buf, uint32_t resp_max, uint32_t* resp_len);

    void CmdGetDisplayInfo(const VirtioGpuCtrlHdr* hdr,
                           uint8_t* resp, uint32_t* resp_len);
    void CmdResourceCreate2d(const uint8_t* req, uint32_t req_len,
                             uint8_t* resp, uint32_t* resp_len);
    void CmdResourceUnref(const uint8_t* req, uint32_t req_len,
                          uint8_t* resp, uint32_t* resp_len);
    void CmdSetScanout(const uint8_t* req, uint32_t req_len,
                       uint8_t* resp, uint32_t* resp_len);
    void CmdResourceFlush(const uint8_t* req, uint32_t req_len,
                          uint8_t* resp, uint32_t* resp_len);
    void CmdTransferToHost2d(const uint8_t* req, uint32_t req_len,
                             uint8_t* resp, uint32_t* resp_len);
    void CmdAttachBacking(const uint8_t* req, uint32_t req_len,
                          const uint8_t* extra, uint32_t extra_len,
                          uint8_t* resp, uint32_t* resp_len);
    void CmdDetachBacking(const uint8_t* req, uint32_t req_len,
                          uint8_t* resp, uint32_t* resp_len);

    void WriteResponse(uint8_t* buf, uint32_t type, uint32_t* len);
    uint8_t* GpaToHva(uint64_t gpa) const;

    VirtioMmioDevice* mmio_ = nullptr;
    GuestMemMap mem_{};
    FrameCallback frame_callback_;

    uint32_t display_width_;
    uint32_t display_height_;
    VirtioGpuConfig gpu_config_{};

    std::unordered_map<uint32_t, GpuResource> resources_;
    uint32_t scanout_resource_id_ = 0;
};
