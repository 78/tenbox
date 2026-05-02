#pragma once

#include "daemon/media_interfaces.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace tenbox::daemon {

// Per-slice planar YUV patch produced by the runtime side. Each slice owns its own
// pixel data covering a sub-rectangle of the frame and is applied to the
// encoder's persistent input buffer at (x, y). Multiple slices accumulate
// between drains; the consumer applies all of them in order before encoding.
struct RemoteVideoSlice {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Layout: contiguous Y plane followed by U and V. U/V are half-resolution
    // for YUV420p and full-resolution for YUV444p.
    std::vector<uint8_t> data;
    int strides[3] = {};
};

struct RemoteVideoFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::kYuv420p;
    std::vector<RemoteVideoSlice> slices;
    uint64_t seq = 0;
};

struct RemoteAudioChunk {
    std::vector<int16_t> pcm;
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    int64_t pts_us = 0;
};

// `need_full_frame` asks the producer to discard pending partial slices and
// emit a single full-frame slice (e.g. after the encoder is reopened so its
// internal reference buffer must be re-seeded). When `wait_timeout` is
// non-zero and there are no pending slices, the reader blocks on the
// producer's condition variable until either new slices arrive, the timeout
// expires, or `need_full_frame` lets it synthesize one immediately.
using RemoteFrameReader = std::function<bool(
    RemoteVideoFrame*,
    bool need_full_frame,
    std::chrono::milliseconds wait_timeout)>;

struct WebRtcAnswer {
    bool ok = false;
    std::string sdp;
    nlohmann::json candidates = nlohmann::json::array();
    std::string error;
};

// JSON-shaped message arriving on either the `input-fast` (best-effort) or
// `control` (reliable) DataChannel. Carries `type` + per-type fields, e.g.
// {"type":"pointer","x":..,"y":..,"buttons":..}.
using DataChannelMessageHandler = std::function<void(const nlohmann::json&)>;

// Fired once per DataChannel transitioning to open. Useful for pushing
// "snapshot" state (e.g. current cursor position) that may have been
// produced while the channel was still in DTLS/SCTP setup and thus
// silently dropped by SendOnDataChannel.
using DataChannelOpenHandler = std::function<void(const std::string& label)>;

class WebRtcPeer {
public:
    virtual ~WebRtcPeer() = default;
    virtual WebRtcAnswer AcceptOffer(const std::string& sdp) = 0;
    virtual bool AddIceCandidate(const nlohmann::json& candidate, std::string* error) = 0;
    virtual void PushAudio(RemoteAudioChunk chunk) = 0;
    virtual void SetVideoBitrate(uint32_t bitrate_bps) = 0;
    virtual void SetDataChannelHandler(DataChannelMessageHandler handler) = 0;
    virtual void SetDataChannelOpenHandler(DataChannelOpenHandler handler) = 0;
    // Send a JSON text frame to a specific data channel (typically "control"
    // for clipboard / status events). Returns false if the channel doesn't
    // exist or isn't open yet; the caller is expected to drop the message.
    virtual bool SendOnDataChannel(const std::string& label, const std::string& text) = 0;
};

std::shared_ptr<WebRtcPeer> CreateWebRtcPeer(
    RemoteFrameReader frame_reader = {},
    PixelFormat preferred_video_format = PixelFormat::kYuv420p);
bool NativeWebRtcAvailable();

}  // namespace tenbox::daemon
