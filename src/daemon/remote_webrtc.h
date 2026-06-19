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

// Fired for every host-side ICE candidate as it is gathered. The
// embedder is expected to forward the candidate to the remote peer over
// whatever signaling channel is in use (cloud websocket relay in the
// production setup). Candidates included in the initial WebRtcAnswer
// are *also* delivered through this handler if it is installed before
// AcceptOffer; deduplication is the embedder's responsibility.
using LocalIceCandidateHandler = std::function<void(nlohmann::json candidate)>;

// Fired when the underlying RTCPeerConnection enters a terminal state
// (failed/closed). The embedder is expected to tear down the
// remote-session bookkeeping (drop the peer entry, notify the cloud
// signaling channel) so the browser is told the desktop is gone instead
// of staring at a frozen frame. `reason` is a short tag suitable for
// inclusion in a `remote_session.closed` payload, e.g. "peer_failed".
using PeerClosedHandler = std::function<void(std::string reason)>;

class WebRtcPeer {
public:
    virtual ~WebRtcPeer() = default;
    virtual WebRtcAnswer AcceptOffer(const std::string& sdp) = 0;
    virtual bool AddIceCandidate(const nlohmann::json& candidate, std::string* error) = 0;
    virtual void PushAudio(RemoteAudioChunk chunk) = 0;
    virtual void SetVideoBitrate(uint32_t bitrate_bps) = 0;
    virtual void SetDataChannelHandler(DataChannelMessageHandler handler) = 0;
    virtual void SetDataChannelOpenHandler(DataChannelOpenHandler handler) = 0;
    virtual void SetLocalIceCandidateHandler(LocalIceCandidateHandler handler) = 0;
    virtual void SetPeerClosedHandler(PeerClosedHandler handler) = 0;
    // Send a JSON text frame to a specific data channel (typically "control"
    // for clipboard / status events). Returns false if the channel doesn't
    // exist or isn't open yet; the caller is expected to drop the message.
    virtual bool SendOnDataChannel(const std::string& label, const std::string& text) = 0;
};

std::shared_ptr<WebRtcPeer> CreateWebRtcPeer(
    RemoteFrameReader frame_reader = {},
    PixelFormat preferred_video_format = PixelFormat::kYuv420p);
bool NativeWebRtcAvailable();

// One ICE server entry in the W3C `RTCIceServer`-shaped form we also
// advertise to the browser over the cloud websocket. A single entry can
// carry multiple URLs sharing the same credentials (e.g. the UDP and TCP
// variants of the same TURN host). For pure STUN entries `username`
// and `credential` are empty.
struct IceServerSpec {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

// Returns the resolved ICE server list. Source of truth is (in order):
//   1. AGENTSPHERE_ICE_SERVERS - JSON array of W3C-shaped entries, supports
//      STUN + TURN with credentials.
//   2. AGENTSPHERE_STUN_SERVERS - legacy comma-separated STUN-only URLs.
//   3. Built-in CN-reachable STUN defaults.
// Exposed so the cloud tunnel can advertise the same list to the
// browser-side RTCPeerConnection - keeping both peers probing the same
// set avoids one giving up on srflx because it picked a server the
// other peer cannot reach. See docs/turn-rollout.md in tenbox-cloud for
// the production provisioning plan (TURN credentials are minted by the
// cloud signaling layer, not the daemon).
std::vector<IceServerSpec> ResolvedIceServers();

}  // namespace tenbox::daemon
