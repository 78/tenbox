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

// Cursor commands (spec 5.7.6.8)
constexpr uint32_t VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300;
constexpr uint32_t VIRTIO_GPU_CMD_MOVE_CURSOR   = 0x0301;

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

// Config events (spec 5.7.6.1)
constexpr uint32_t VIRTIO_GPU_EVENT_DISPLAY = 1;

// Control header flags
constexpr uint32_t VIRTIO_GPU_FLAG_FENCE = 1;

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

struct VirtioGpuCursorPos {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
};

struct VirtioGpuUpdateCursor {
    VirtioGpuCtrlHdr hdr;
    VirtioGpuCursorPos pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
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
    using FrameCallback = std::function<void(DisplayFrame)>;
    using CursorCallback = std::function<void(const CursorInfo&)>;
    using ScanoutStateCallback = std::function<void(bool active, uint32_t width, uint32_t height)>;

    VirtioGpuDevice(uint32_t width, uint32_t height);
    ~VirtioGpuDevice() override = default;

    void SetMmioDevice(VirtioMmioDevice* mmio) { mmio_ = mmio; }
    void SetMemMap(const GuestMemMap& mem) { mem_ = mem; }
    void SetFrameCallback(FrameCallback cb) { frame_callback_ = std::move(cb); }
    void SetCursorCallback(CursorCallback cb) { cursor_callback_ = std::move(cb); }
    void SetScanoutStateCallback(ScanoutStateCallback cb) { scanout_state_callback_ = std::move(cb); }

    // Update display resolution and notify guest to re-query display info
    void SetDisplaySize(uint32_t width, uint32_t height);

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
    void ProcessCursorQueue(VirtQueue& vq);

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
    void CopyFromBacking(const std::vector<GpuResource::BackingPage>& backing,
                         uint64_t offset, uint32_t length, uint8_t* dst) const;

    VirtioMmioDevice* mmio_ = nullptr;
    GuestMemMap mem_{};
    FrameCallback frame_callback_;
    CursorCallback cursor_callback_;
    ScanoutStateCallback scanout_state_callback_;

    uint32_t display_width_;
    uint32_t display_height_;
    VirtioGpuConfig gpu_config_{};

    std::unordered_map<uint32_t, GpuResource> resources_;
    uint32_t scanout_resource_id_ = 0;
    uint32_t scanout_width_ = 0;
    uint32_t scanout_height_ = 0;

    // Cursor state
    uint32_t cursor_resource_id_ = 0;
    int32_t cursor_x_ = 0;
    int32_t cursor_y_ = 0;
    uint32_t cursor_hot_x_ = 0;
    uint32_t cursor_hot_y_ = 0;

    // Snapshot of what we last delivered through `cursor_callback_`. Guests
    // (notably the Linux virtio_gpu driver) re-emit MOVE_CURSOR every vblank
    // even while the pointer is stationary, so without this dedup we would
    // re-encode and re-send identical frames at the display refresh rate.
    // UPDATE_CURSOR always passes through because the same resource_id can
    // carry freshly re-uploaded pixels via TRANSFER_TO_HOST_2D.
    struct LastEmittedCursor {
        bool valid = false;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t hot_x = 0;
        uint32_t hot_y = 0;
        uint32_t resource_id = 0;
        bool visible = false;

        // True when the new state matches what we already pushed downstream;
        // in that case the caller can drop the event entirely.
        bool MatchesMove(int32_t new_x,
                         int32_t new_y,
                         uint32_t new_hot_x,
                         uint32_t new_hot_y,
                         uint32_t new_resource_id,
                         bool new_visible) const {
            return valid &&
                   x == new_x &&
                   y == new_y &&
                   hot_x == new_hot_x &&
                   hot_y == new_hot_y &&
                   resource_id == new_resource_id &&
                   visible == new_visible;
        }

        void Update(int32_t new_x,
                    int32_t new_y,
                    uint32_t new_hot_x,
                    uint32_t new_hot_y,
                    uint32_t new_resource_id,
                    bool new_visible) {
            valid = true;
            x = new_x;
            y = new_y;
            hot_x = new_hot_x;
            hot_y = new_hot_y;
            resource_id = new_resource_id;
            visible = new_visible;
        }
    };
    LastEmittedCursor last_emitted_cursor_;
};
