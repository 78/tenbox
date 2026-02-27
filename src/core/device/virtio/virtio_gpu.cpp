#include "core/device/virtio/virtio_gpu.h"
#include "core/vmm/types.h"
#include <algorithm>
#include <cstring>

#ifndef VIRTIO_F_VERSION_1_DEFINED
#define VIRTIO_F_VERSION_1_DEFINED
constexpr uint64_t VIRTIO_GPU_VER1 = 1ULL << 32;
#else
static constexpr uint64_t VIRTIO_GPU_VER1 = 1ULL << 32;
#endif

static uint32_t FormatBpp(uint32_t format) {
    switch (format) {
    case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
    case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
    case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
    case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
    case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
    case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
    case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
    case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
        return 4;
    default:
        return 4;
    }
}

VirtioGpuDevice::VirtioGpuDevice(uint32_t width, uint32_t height)
    : display_width_(width), display_height_(height) {
    gpu_config_.events_read = 0;
    gpu_config_.events_clear = 0;
    gpu_config_.num_scanouts = 1;
    gpu_config_.num_capsets = 0;
}

uint64_t VirtioGpuDevice::GetDeviceFeatures() const {
    return VIRTIO_GPU_VER1;
}

void VirtioGpuDevice::ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) {
    const auto* cfg = reinterpret_cast<const uint8_t*>(&gpu_config_);
    if (offset + size > sizeof(gpu_config_)) {
        *value = 0;
        return;
    }
    *value = 0;
    std::memcpy(value, cfg + offset, size);
}

void VirtioGpuDevice::WriteConfig(uint32_t offset, uint8_t size, uint32_t value) {
    // Only events_clear (offset 4) is writable
    if (offset == 4 && size == 4) {
        gpu_config_.events_read &= ~value;
    }
}

void VirtioGpuDevice::OnStatusChange(uint32_t new_status) {
    if (new_status == 0) {
        resources_.clear();
        scanout_resource_id_ = 0;
    }
}

void VirtioGpuDevice::OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) {
    if (queue_idx == 0) {
        ProcessControlQueue(vq);
    } else if (queue_idx == 1) {
        ProcessCursorQueue(vq);
    }
}

uint8_t* VirtioGpuDevice::GpaToHva(uint64_t gpa) const {
    if (!mem_.base) return nullptr;
    if (gpa < mem_.low_size) {
        return mem_.base + gpa;
    }
    if (mem_.high_size && gpa >= mem_.high_base &&
        gpa < mem_.high_base + mem_.high_size) {
        return mem_.base + mem_.low_size + (gpa - mem_.high_base);
    }
    return nullptr;
}

void VirtioGpuDevice::ProcessControlQueue(VirtQueue& vq) {
    uint16_t head;
    while (vq.PopAvail(&head)) {
        std::vector<VirtqChainElem> chain;
        if (!vq.WalkChain(head, &chain)) {
            vq.PushUsed(head, 0);
            continue;
        }

        // Collect readable (request) and writable (response) buffers
        std::vector<uint8_t> req_buf;
        std::vector<VirtqChainElem> resp_elems;

        for (auto& elem : chain) {
            if (!elem.writable) {
                req_buf.insert(req_buf.end(), elem.addr, elem.addr + elem.len);
            } else {
                resp_elems.push_back(elem);
            }
        }

        if (req_buf.size() < sizeof(VirtioGpuCtrlHdr)) {
            vq.PushUsed(head, 0);
            continue;
        }

        // Prepare response buffer (max typical size)
        std::vector<uint8_t> resp(4096, 0);
        uint32_t resp_len = 0;

        auto* hdr = reinterpret_cast<const VirtioGpuCtrlHdr*>(req_buf.data());

        switch (hdr->type) {
        case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
            CmdGetDisplayInfo(hdr, resp.data(), &resp_len);
            break;
        case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
            CmdResourceCreate2d(req_buf.data(), static_cast<uint32_t>(req_buf.size()),
                                resp.data(), &resp_len);
            break;
        case VIRTIO_GPU_CMD_RESOURCE_UNREF:
            CmdResourceUnref(req_buf.data(), static_cast<uint32_t>(req_buf.size()),
                             resp.data(), &resp_len);
            break;
        case VIRTIO_GPU_CMD_SET_SCANOUT:
            CmdSetScanout(req_buf.data(), static_cast<uint32_t>(req_buf.size()),
                          resp.data(), &resp_len);
            break;
        case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
            CmdResourceFlush(req_buf.data(), static_cast<uint32_t>(req_buf.size()),
                             resp.data(), &resp_len);
            break;
        case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
            CmdTransferToHost2d(req_buf.data(), static_cast<uint32_t>(req_buf.size()),
                                resp.data(), &resp_len);
            break;
        case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: {
            // The attach backing command header is followed by mem entries
            // which may span into subsequent readable descriptors (already
            // concatenated into req_buf).
            const uint8_t* extra = nullptr;
            uint32_t extra_len = 0;
            if (req_buf.size() > sizeof(VirtioGpuResourceAttachBacking)) {
                extra = req_buf.data() + sizeof(VirtioGpuResourceAttachBacking);
                extra_len = static_cast<uint32_t>(
                    req_buf.size() - sizeof(VirtioGpuResourceAttachBacking));
            }
            CmdAttachBacking(req_buf.data(), static_cast<uint32_t>(req_buf.size()),
                             extra, extra_len, resp.data(), &resp_len);
            break;
        }
        case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
            CmdDetachBacking(req_buf.data(), static_cast<uint32_t>(req_buf.size()),
                             resp.data(), &resp_len);
            break;
        default:
            WriteResponse(resp.data(), VIRTIO_GPU_RESP_ERR_UNSPEC, &resp_len);
            break;
        }

        // If the request had VIRTIO_GPU_FLAG_FENCE set, copy fence info to response.
        // The guest driver waits on dma_fence which is signaled only when the
        // response contains matching flags and fence_id.
        if (resp_len >= sizeof(VirtioGpuCtrlHdr) &&
            (hdr->flags & VIRTIO_GPU_FLAG_FENCE)) {
            auto* resp_hdr = reinterpret_cast<VirtioGpuCtrlHdr*>(resp.data());
            resp_hdr->flags |= VIRTIO_GPU_FLAG_FENCE;
            resp_hdr->fence_id = hdr->fence_id;
            resp_hdr->ctx_id = hdr->ctx_id;
        }

        // Copy response into writable descriptors
        uint32_t written = 0;
        for (auto& elem : resp_elems) {
            if (written >= resp_len) break;
            uint32_t to_copy = (std::min)(elem.len, resp_len - written);
            std::memcpy(elem.addr, resp.data() + written, to_copy);
            written += to_copy;
        }

        vq.PushUsed(head, written);
    }

    if (mmio_) mmio_->NotifyUsedBuffer();
}

void VirtioGpuDevice::ProcessCursorQueue(VirtQueue& vq) {
    uint16_t head;
    while (vq.PopAvail(&head)) {
        std::vector<VirtqChainElem> chain;
        if (!vq.WalkChain(head, &chain)) {
            vq.PushUsed(head, 0);
            continue;
        }

        // Cursor commands typically don't have response buffers, but we need
        // to consume the request to unblock the guest. Parse the command type
        // for potential future cursor rendering support.
        std::vector<uint8_t> req_buf;
        for (auto& elem : chain) {
            if (!elem.writable) {
                req_buf.insert(req_buf.end(), elem.addr, elem.addr + elem.len);
            }
        }

        if (req_buf.size() >= sizeof(VirtioGpuCtrlHdr)) {
            auto* hdr = reinterpret_cast<const VirtioGpuCtrlHdr*>(req_buf.data());
            (void)hdr; // Cursor commands (UPDATE_CURSOR, MOVE_CURSOR) are acknowledged
                       // by simply consuming them. Hardware cursor rendering is optional.
        }

        // Mark the buffer as used (no response data for cursor commands)
        vq.PushUsed(head, 0);
    }

    if (mmio_) mmio_->NotifyUsedBuffer();
}

void VirtioGpuDevice::WriteResponse(uint8_t* buf, uint32_t type, uint32_t* len) {
    auto* hdr = reinterpret_cast<VirtioGpuCtrlHdr*>(buf);
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->type = type;
    *len = sizeof(VirtioGpuCtrlHdr);
}

void VirtioGpuDevice::CmdGetDisplayInfo(const VirtioGpuCtrlHdr* hdr,
                                         uint8_t* resp, uint32_t* resp_len) {
    auto* info = reinterpret_cast<VirtioGpuRespDisplayInfo*>(resp);
    std::memset(info, 0, sizeof(*info));
    info->hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
    info->pmodes[0].r.x = 0;
    info->pmodes[0].r.y = 0;
    info->pmodes[0].r.width = display_width_;
    info->pmodes[0].r.height = display_height_;
    info->pmodes[0].enabled = 1;
    *resp_len = sizeof(VirtioGpuRespDisplayInfo);
}

void VirtioGpuDevice::CmdResourceCreate2d(const uint8_t* req, uint32_t req_len,
                                            uint8_t* resp, uint32_t* resp_len) {
    if (req_len < sizeof(VirtioGpuResourceCreate2d)) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }
    auto* cmd = reinterpret_cast<const VirtioGpuResourceCreate2d*>(req);
    if (cmd->resource_id == 0 || cmd->width == 0 || cmd->height == 0 ||
        cmd->width > 16384 || cmd->height > 16384) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }

    GpuResource res;
    res.id = cmd->resource_id;
    res.width = cmd->width;
    res.height = cmd->height;
    res.format = cmd->format;
    res.host_pixels.resize(static_cast<size_t>(cmd->width) * cmd->height * FormatBpp(cmd->format), 0);

    resources_[cmd->resource_id] = std::move(res);
    WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
}

void VirtioGpuDevice::CmdResourceUnref(const uint8_t* req, uint32_t req_len,
                                         uint8_t* resp, uint32_t* resp_len) {
    if (req_len < sizeof(VirtioGpuResourceUnref)) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }
    auto* cmd = reinterpret_cast<const VirtioGpuResourceUnref*>(req);
    auto it = resources_.find(cmd->resource_id);
    if (it == resources_.end()) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID, resp_len);
        return;
    }
    if (scanout_resource_id_ == cmd->resource_id) {
        scanout_resource_id_ = 0;
    }
    resources_.erase(it);
    WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
}

void VirtioGpuDevice::CmdSetScanout(const uint8_t* req, uint32_t req_len,
                                      uint8_t* resp, uint32_t* resp_len) {
    if (req_len < sizeof(VirtioGpuSetScanout)) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }
    auto* cmd = reinterpret_cast<const VirtioGpuSetScanout*>(req);
    if (cmd->scanout_id != 0) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }
    if (cmd->resource_id == 0) {
        scanout_resource_id_ = 0;
        WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
        return;
    }
    if (resources_.find(cmd->resource_id) == resources_.end()) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID, resp_len);
        return;
    }
    scanout_resource_id_ = cmd->resource_id;
    WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
}

void VirtioGpuDevice::CmdTransferToHost2d(const uint8_t* req, uint32_t req_len,
                                            uint8_t* resp, uint32_t* resp_len) {
    if (req_len < sizeof(VirtioGpuTransferToHost2d)) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }
    auto* cmd = reinterpret_cast<const VirtioGpuTransferToHost2d*>(req);
    auto it = resources_.find(cmd->resource_id);
    if (it == resources_.end()) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID, resp_len);
        return;
    }

    auto& res = it->second;
    uint32_t bpp = FormatBpp(res.format);
    uint32_t stride = res.width * bpp;

    // Linearize the backing pages into the host pixel buffer
    // The transfer rectangle specifies which region to copy.
    uint32_t rx = cmd->r.x;
    uint32_t ry = cmd->r.y;
    uint32_t rw = cmd->r.width;
    uint32_t rh = cmd->r.height;

    if (rx >= res.width || ry >= res.height) {
        WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
        return;
    }
    if (rx + rw > res.width) rw = res.width - rx;
    if (ry + rh > res.height) rh = res.height - ry;

    // Build a linear view of all backing pages
    std::vector<uint8_t> linear;
    for (auto& page : res.backing) {
        uint8_t* hva = GpaToHva(page.gpa);
        if (hva) {
            linear.insert(linear.end(), hva, hva + page.length);
        } else {
            linear.resize(linear.size() + page.length, 0);
        }
    }

    // Copy the requested rectangle from the linear backing to host_pixels
    uint64_t src_offset = cmd->offset;
    for (uint32_t row = 0; row < rh; ++row) {
        uint64_t src_row_off = src_offset + static_cast<uint64_t>(row) * stride;
        uint64_t dst_off = (static_cast<uint64_t>(ry + row) * stride) +
                           (static_cast<uint64_t>(rx) * bpp);
        uint32_t row_bytes = rw * bpp;

        if (src_row_off + row_bytes > linear.size()) break;
        if (dst_off + row_bytes > res.host_pixels.size()) break;

        std::memcpy(res.host_pixels.data() + dst_off,
                    linear.data() + src_row_off,
                    row_bytes);
    }

    WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
}

void VirtioGpuDevice::CmdResourceFlush(const uint8_t* req, uint32_t req_len,
                                         uint8_t* resp, uint32_t* resp_len) {
    if (req_len < sizeof(VirtioGpuResourceFlush)) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }
    auto* cmd = reinterpret_cast<const VirtioGpuResourceFlush*>(req);
    auto it = resources_.find(cmd->resource_id);
    if (it == resources_.end()) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID, resp_len);
        return;
    }

    // Only flush if this resource is the active scanout
    if (cmd->resource_id == scanout_resource_id_ && frame_callback_) {
        auto& res = it->second;
        uint32_t bpp = FormatBpp(res.format);
        uint32_t full_stride = res.width * bpp;

        uint32_t dx = cmd->r.x;
        uint32_t dy = cmd->r.y;
        uint32_t dw = cmd->r.width;
        uint32_t dh = cmd->r.height;

        if (dx >= res.width || dy >= res.height) {
            WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
            return;
        }
        if (dx + dw > res.width) dw = res.width - dx;
        if (dy + dh > res.height) dh = res.height - dy;

        DisplayFrame frame;
        frame.width = dw;
        frame.height = dh;
        frame.stride = dw * bpp;
        frame.format = res.format;
        frame.resource_width = res.width;
        frame.resource_height = res.height;
        frame.dirty_x = dx;
        frame.dirty_y = dy;
        frame.pixels.resize(static_cast<size_t>(dw) * dh * bpp);

        for (uint32_t row = 0; row < dh; ++row) {
            uint64_t src = (static_cast<uint64_t>(dy + row) * full_stride) +
                           (static_cast<uint64_t>(dx) * bpp);
            uint64_t dst = static_cast<uint64_t>(row) * dw * bpp;
            if (src + dw * bpp > res.host_pixels.size()) break;
            std::memcpy(frame.pixels.data() + dst,
                        res.host_pixels.data() + src,
                        dw * bpp);
        }

        frame_callback_(frame);
    }

    WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
}

void VirtioGpuDevice::CmdAttachBacking(const uint8_t* req, uint32_t req_len,
                                         const uint8_t* extra, uint32_t extra_len,
                                         uint8_t* resp, uint32_t* resp_len) {
    if (req_len < sizeof(VirtioGpuResourceAttachBacking)) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }
    auto* cmd = reinterpret_cast<const VirtioGpuResourceAttachBacking*>(req);
    auto it = resources_.find(cmd->resource_id);
    if (it == resources_.end()) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID, resp_len);
        return;
    }

    auto& res = it->second;
    res.backing.clear();

    uint32_t nr = cmd->nr_entries;
    if (nr > 16384) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }
    if (extra && extra_len >= nr * sizeof(VirtioGpuMemEntry)) {
        auto* entries = reinterpret_cast<const VirtioGpuMemEntry*>(extra);
        for (uint32_t i = 0; i < nr; ++i) {
            if (entries[i].length == 0 || entries[i].length > 64 * 1024 * 1024) {
                res.backing.clear();
                WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
                return;
            }
            res.backing.push_back({entries[i].addr, entries[i].length});
        }
    }

    WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
}

void VirtioGpuDevice::CmdDetachBacking(const uint8_t* req, uint32_t req_len,
                                         uint8_t* resp, uint32_t* resp_len) {
    if (req_len < sizeof(VirtioGpuResourceDetachBacking)) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, resp_len);
        return;
    }
    auto* cmd = reinterpret_cast<const VirtioGpuResourceDetachBacking*>(req);
    auto it = resources_.find(cmd->resource_id);
    if (it == resources_.end()) {
        WriteResponse(resp, VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID, resp_len);
        return;
    }
    it->second.backing.clear();
    WriteResponse(resp, VIRTIO_GPU_RESP_OK_NODATA, resp_len);
}
