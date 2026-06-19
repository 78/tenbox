#include "daemon/remote_webrtc.h"
#include "daemon/media_interfaces.h"

#include <rtc/rtc.hpp>
#include <rtc/plihandler.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <charconv>
#include <cstring>
#include <deque>
#include <exception>
#include <inttypes.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <pthread.h>
#include <thread>
#include <utility>
#include <vector>

namespace tenbox::daemon {

namespace {

bool PreferConstrainedBaselineH264() {
    const char* value = std::getenv("AGENTSPHERE_WEBRTC_H264_PROFILE");
    return value && std::string_view(value) == "baseline";
}

// Verbose webrtc logging is off by default to keep the daemon log readable
// in production. Set AGENTSPHERE_WEBRTC_VERBOSE=1 (or build with _DEBUG) to get
// every SDP payload negotiation, data channel attach/close, peer state
// transition (other than the terminal ones we always log), keyframe PLI
// from the receiver, etc. Errors / warnings / first-time encoder open
// remain at the default level regardless of this flag.
bool VerboseWebRtcLogging() {
#ifdef _DEBUG
    return true;
#else
    static const bool enabled = []() {
        const char* value = std::getenv("AGENTSPHERE_WEBRTC_VERBOSE");
        return value && value[0] != '\0' && std::string_view(value) != "0";
    }();
    return enabled;
#endif
}

// Like fprintf(stdout, ...) but no-op unless VerboseWebRtcLogging() is on.
// Use for events that fire on every session attach / SDP negotiation or
// faster (PLI, data channel lifecycle); the production user does not care
// and the log is otherwise dominated by them after a few reconnects.
template <typename... Args>
void VerboseLog(const char* fmt, Args&&... args) {
    if (!VerboseWebRtcLogging()) return;
    std::fprintf(stdout, fmt, std::forward<Args>(args)...);
    std::fflush(stdout);
}

const char* PeerStateName(int state) {
    // Mirrors libdatachannel's rtc::PeerConnection::State enum order.
    switch (state) {
        case 0: return "new";
        case 1: return "connecting";
        case 2: return "connected";
        case 3: return "disconnected";
        case 4: return "failed";
        case 5: return "closed";
        default: return "unknown";
    }
}

// Parses the legacy AGENTSPHERE_STUN_SERVERS env format: comma-separated
// (whitespace tolerated) URLs like
// "stun:stun.qq.com:3478,stun:stun.miwifi.com:3478". Empty entries
// are skipped. Used both for the dedicated STUN-only env var and as a
// last-resort tokenizer for anything that slipped past JSON parsing.
std::vector<std::string> SplitCommaSeparated(std::string_view view) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= view.size()) {
        size_t comma = view.find(',', start);
        size_t end = comma == std::string_view::npos ? view.size() : comma;
        size_t s = start;
        size_t e = end;
        while (s < e && std::isspace(static_cast<unsigned char>(view[s]))) ++s;
        while (e > s && std::isspace(static_cast<unsigned char>(view[e - 1]))) --e;
        if (e > s) out.emplace_back(view.substr(s, e - s));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

// Built-in defaults used when neither AGENTSPHERE_ICE_SERVERS nor
// AGENTSPHERE_STUN_SERVERS is set. Defaults are skewed for mainland China
// deployments because that's where most of the production fleet lives
// today and Google STUN (stun.l.google.com:19302) is regularly
// UDP-blackholed by CN ISPs, which previously made WebRTC setup time
// out at "creating answer". Order matters - libdatachannel probes them
// in sequence:
//   1. Tencent  - rock solid in CN, used by every domestic Live SDK
//   2. Xiaomi   - secondary domestic option, embedded in MiWiFi routers
//   3. Cloudflare - global fallback for users outside CN; CF anycast is
//                   reachable with low latency from the mainland too,
//                   so it doubles as a third tier when the first two are
//                   busy or filtered.
IceServerSpec DefaultStunServers() {
    IceServerSpec spec;
    spec.urls = {
        "stun:stun.qq.com:3478",
        "stun:stun.miwifi.com:3478",
        "stun:stun.cloudflare.com:3478",
    };
    return spec;
}

// Parses AGENTSPHERE_ICE_SERVERS, a JSON array of W3C-shaped RTCIceServer
// dictionaries:
//
//   [
//     {"urls":["stun:stun.qq.com:3478"]},
//     {"urls":["turn:turn.tenbox.ai:3478?transport=udp",
//              "turn:turn.tenbox.ai:3478?transport=tcp"],
//      "username":"1735689600:u_42:s_abc",
//      "credential":"base64-hmac-sha1"}
//   ]
//
// `urls` may also be a single string (matching the W3C union type).
// Entries with zero usable URLs are dropped silently.
//
// Credentials are almost always unused on the daemon side - in
// production only the browser peer receives TURN credentials, because
// a WebRTC session succeeds as long as *one* peer contributes a relay
// candidate and the daemon is rarely the constrained side. The schema
// still parses them so the same env var works for self-hosted
// operators who want the daemon to relay through their own TURN.
std::optional<std::vector<IceServerSpec>> ParseIceServersJson(std::string_view raw) {
    std::vector<IceServerSpec> out;
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(raw);
    } catch (const std::exception& e) {
        std::fprintf(stdout,
                     "[WARN]  remote_webrtc: AGENTSPHERE_ICE_SERVERS is not valid JSON: %s\n",
                     e.what());
        std::fflush(stdout);
        return std::nullopt;
    }
    if (!parsed.is_array()) {
        std::fprintf(stdout,
                     "[WARN]  remote_webrtc: AGENTSPHERE_ICE_SERVERS must be a JSON array\n");
        std::fflush(stdout);
        return std::nullopt;
    }
    for (const auto& entry : parsed) {
        if (!entry.is_object()) continue;
        IceServerSpec spec;
        if (auto it = entry.find("urls"); it != entry.end()) {
            if (it->is_string()) {
                spec.urls.push_back(it->get<std::string>());
            } else if (it->is_array()) {
                for (const auto& u : *it) {
                    if (u.is_string()) spec.urls.push_back(u.get<std::string>());
                }
            }
        }
        if (spec.urls.empty()) continue;
        if (auto it = entry.find("username"); it != entry.end() && it->is_string()) {
            spec.username = it->get<std::string>();
        }
        if (auto it = entry.find("credential"); it != entry.end() && it->is_string()) {
            spec.credential = it->get<std::string>();
        }
        out.push_back(std::move(spec));
    }
    return out;
}

std::vector<IceServerSpec> ConfiguredIceServers() {
    if (const char* raw = std::getenv("AGENTSPHERE_ICE_SERVERS"); raw && raw[0] != '\0') {
        if (auto parsed = ParseIceServersJson(raw); parsed && !parsed->empty()) {
            return std::move(*parsed);
        }
        // Fall through to the legacy env var / defaults rather than
        // returning an empty list - an operator who sets a broken
        // JSON value should still get a working daemon.
    }
    if (const char* raw = std::getenv("AGENTSPHERE_STUN_SERVERS"); raw && raw[0] != '\0') {
        IceServerSpec spec;
        spec.urls = SplitCommaSeparated(raw);
        if (!spec.urls.empty()) return {std::move(spec)};
    }
    return {DefaultStunServers()};
}

// Wraps rtc::IceServer construction with a fallback for STUN URLs
// (which take just a URL) vs TURN URLs (which need a
// hostname/port/user/pass tuple). libdatachannel's single-arg
// IceServer(url) constructor already parses "turn://user:pass@host:port"
// but does NOT accept the W3C-style "turn:host:port" with a separate
// username/credential, so we peel those apart manually.
std::optional<rtc::IceServer> BuildRtcIceServer(const std::string& url,
                                                const std::string& username,
                                                const std::string& credential) {
    if (username.empty() && credential.empty()) {
        return rtc::IceServer(url);
    }
    // TURN URL shape: "turn[s]:host[:port][?transport=udp|tcp]". Strip
    // the scheme so IceServer's hostname/service constructor can chew
    // on what's left. Any URL that isn't recognisably TURN falls back
    // to the single-arg constructor.
    auto starts_with = [](std::string_view s, std::string_view p) {
        return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
    };
    std::string_view body(url);
    auto relay_type = rtc::IceServer::RelayType::TurnUdp;
    if (starts_with(body, "turns:")) {
        body.remove_prefix(6);
        relay_type = rtc::IceServer::RelayType::TurnTls;
    } else if (starts_with(body, "turn:")) {
        body.remove_prefix(5);
    } else {
        return rtc::IceServer(url);
    }
    auto qmark = body.find('?');
    std::string_view host_port = qmark == std::string_view::npos ? body : body.substr(0, qmark);
    if (qmark != std::string_view::npos) {
        std::string_view query = body.substr(qmark + 1);
        if (query.find("transport=tcp") != std::string_view::npos) {
            // Keep TLS relay type for turns:, otherwise switch to TCP.
            if (relay_type != rtc::IceServer::RelayType::TurnTls) {
                relay_type = rtc::IceServer::RelayType::TurnTcp;
            }
        }
    }
    auto colon = host_port.rfind(':');
    std::string host;
    std::string service;
    if (colon == std::string_view::npos) {
        host = std::string(host_port);
        service = (relay_type == rtc::IceServer::RelayType::TurnTls) ? "5349" : "3478";
    } else {
        host = std::string(host_port.substr(0, colon));
        service = std::string(host_port.substr(colon + 1));
    }
    if (host.empty()) return std::nullopt;
    return rtc::IceServer(std::move(host), std::move(service), username, credential, relay_type);
}

std::string StripLipSyncGroups(std::string sdp) {
    std::string filtered;
    filtered.reserve(sdp.size());

    size_t pos = 0;
    while (pos < sdp.size()) {
        const size_t eol = sdp.find('\n', pos);
        const size_t next = eol == std::string::npos ? sdp.size() : eol + 1;
        size_t line_end = eol == std::string::npos ? sdp.size() : eol;
        if (line_end > pos && sdp[line_end - 1] == '\r') --line_end;

        const std::string_view line(sdp.data() + pos, line_end - pos);
        const bool is_lipsync_group =
            line.size() >= 10 &&
            line.compare(0, 10, "a=group:LS") == 0 &&
            (line.size() == 10 || line[10] == ' ' || line[10] == '\t');
        if (!is_lipsync_group) filtered.append(sdp, pos, next - pos);
        pos = next;
    }

    return filtered;
}

}  // namespace

// Wire libdatachannel's internal logger into our stdout the first time
// any peer is created. We only enable the chatty levels (Debug, Verbose)
// when AGENTSPHERE_WEBRTC_VERBOSE is on; otherwise libdatachannel still
// surfaces warnings/errors so juice STUN failures, ICE state machine
// transitions, etc. show up in the daemon journal during connectivity
// debugging without requiring a code change. Initialization is one-shot
// and idempotent under std::call_once so we never re-register the sink.
void EnsureLibDatachannelLoggerInstalled() {
    static std::once_flag once;
    std::call_once(once, []() {
        const auto level = VerboseWebRtcLogging() ? rtc::LogLevel::Debug : rtc::LogLevel::Info;
        rtc::InitLogger(level, [](rtc::LogLevel lvl, std::string message) {
            const char* tag = "INFO ";
            switch (lvl) {
                case rtc::LogLevel::Fatal:   tag = "FATAL"; break;
                case rtc::LogLevel::Error:   tag = "ERROR"; break;
                case rtc::LogLevel::Warning: tag = "WARN "; break;
                case rtc::LogLevel::Info:    tag = "INFO "; break;
                case rtc::LogLevel::Debug:   tag = "DEBUG"; break;
                case rtc::LogLevel::Verbose: tag = "TRACE"; break;
                default: break;
            }
            std::fprintf(stdout, "[%s] libdatachannel: %s\n", tag, message.c_str());
            std::fflush(stdout);
        });
        // Default is hardware_concurrency() which is excessive for a daemon
        // handling a handful of concurrent remote desktop sessions.
        // Override with AGENTSPHERE_WEBRTC_WORKER_THREADS=0 to restore the default.
        unsigned int pool_size = 4;
        if (const char* v = std::getenv("AGENTSPHERE_WEBRTC_WORKER_THREADS"); v && v[0] != '\0') {
            const auto result = std::from_chars(v, v + std::strlen(v), pool_size);
            if (result.ec != std::errc{}) pool_size = 4;
        }
        rtc::SetThreadPoolSize(pool_size);
    });
}

class NativeWebRtcPeer final
    : public WebRtcPeer,
      public std::enable_shared_from_this<NativeWebRtcPeer> {
public:
    explicit NativeWebRtcPeer(RemoteFrameReader frame_reader, PixelFormat preferred_video_format)
        : frame_reader_(std::move(frame_reader)),
          preferred_video_format_(preferred_video_format) {
        EnsureLibDatachannelLoggerInstalled();
        rtc::Configuration config;
        for (const auto& spec : ConfiguredIceServers()) {
            for (const auto& url : spec.urls) {
                try {
                    auto built = BuildRtcIceServer(url, spec.username, spec.credential);
                    if (built) {
                        config.iceServers.push_back(std::move(*built));
                    } else {
                        std::fprintf(stdout,
                                     "[WARN]  remote_webrtc: ignoring malformed ICE url %s\n",
                                     url.c_str());
                        std::fflush(stdout);
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stdout,
                                 "[WARN]  remote_webrtc: ignoring invalid ICE url %s: %s\n",
                                 url.c_str(), e.what());
                    std::fflush(stdout);
                }
            }
        }
        config.disableAutoNegotiation = true;
        peer_ = std::make_shared<rtc::PeerConnection>(std::move(config));
    }

    void InstallCallbacks() {
        std::weak_ptr<NativeWebRtcPeer> weak = shared_from_this();
        peer_->onStateChange([weak](rtc::PeerConnection::State state) {
            if (auto self = weak.lock()) self->HandleStateChange(state);
        });
        peer_->onLocalDescription([weak](rtc::Description description) {
            if (auto self = weak.lock()) self->HandleLocalDescription(std::move(description));
        });
        peer_->onLocalCandidate([weak](rtc::Candidate candidate) {
            if (auto self = weak.lock()) self->HandleLocalCandidate(std::move(candidate));
        });
        peer_->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState state) {
            if (auto self = weak.lock()) self->HandleGatheringStateChange(state);
        });
        peer_->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> channel) {
            if (auto self = weak.lock()) self->AttachDataChannel(std::move(channel));
        });
        peer_->onTrack([weak](std::shared_ptr<rtc::Track> track) {
            if (auto self = weak.lock()) self->HandleTrack(std::move(track));
        });
    }

    void HandleStateChange(rtc::PeerConnection::State state) {
        const int code = static_cast<int>(state);
        // Connecting/connected fire on every session and reconnect; only
        // the terminal-ish transitions (disconnected/failed/closed) are
        // worth logging at INFO. The rest goes to verbose.
        if (code >= 3) {
            std::fprintf(stdout,
                         "[INFO]  remote_webrtc: peer state %s\n",
                         PeerStateName(code));
            std::fflush(stdout);
        } else {
            VerboseLog("[INFO]  remote_webrtc: peer state %s\n",
                       PeerStateName(code));
        }
        if (state == rtc::PeerConnection::State::Connected) {
            StartVideoPump();
            StartAudioPump();
        }
        // Surface terminal transitions to the embedder so it can drop
        // the per-session bookkeeping and tell the browser the
        // desktop is gone. We deliberately ignore Disconnected
        // because libdatachannel will either climb back to Connected
        // or escalate to Failed on its own ICE consent timer; firing
        // on Disconnected would tear sessions down for transient
        // wifi blips. `peer_closed_dispatched_` keeps us idempotent
        // because Failed -> Closed often fires both edges.
        if (state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            PeerClosedHandler handler;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (peer_closed_dispatched_) return;
                peer_closed_dispatched_ = true;
                handler = peer_closed_handler_;
            }
            if (handler) {
                const std::string reason =
                    state == rtc::PeerConnection::State::Failed ? "peer_failed" : "peer_closed";
                try {
                    handler(reason);
                } catch (const std::exception& e) {
                    std::fprintf(stdout,
                                 "[ERROR] remote_webrtc: peer closed handler threw: %s\n",
                                 e.what());
                    std::fflush(stdout);
                }
            }
        }
    }

    void HandleLocalDescription(rtc::Description description) {
        std::lock_guard<std::mutex> lock(mu_);
        local_type_ = description.typeString();
        local_sdp_ = StripLipSyncGroups(std::string(description));
        description_ready_ = true;
        cv_.notify_all();
    }

    void HandleLocalCandidate(rtc::Candidate candidate) {
        nlohmann::json entry = {
            {"candidate", std::string(candidate)},
            {"sdpMid", candidate.mid()},
        };
        LocalIceCandidateHandler handler;
        {
            std::lock_guard<std::mutex> lock(mu_);
            candidates_.push_back(entry);
            handler = local_ice_handler_;
        }
        // Trickle every host-side candidate to the embedder so the
        // browser can start probing as soon as gathering produces
        // host / srflx entries, instead of having to wait for the
        // initial answer's `candidates[]` (which we cap at a short
        // gathering window so STUN-blackholed networks don't stall
        // session creation).
        //
        // Trickle frames may legitimately reach the browser before
        // the answer SDP - libdatachannel's worker can fire
        // onLocalCandidate before our caller has had a chance to
        // SendJson the answer envelope. That's the standard
        // signaling race every WebRTC client is expected to
        // handle by queueing addIceCandidate calls until
        // setRemoteDescription resolves; the browser-side
        // RemoteDesktopPanel does exactly that, so we fire as
        // soon as we have a candidate.
        if (handler) {
            try {
                handler(std::move(entry));
            } catch (const std::exception& e) {
                std::fprintf(stdout,
                             "[ERROR] remote_webrtc: local ice handler threw: %s\n",
                             e.what());
                std::fflush(stdout);
            }
        }
    }

    void HandleGatheringStateChange(rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            std::lock_guard<std::mutex> lock(mu_);
            gathering_complete_ = true;
            cv_.notify_all();
        }
    }

    void HandleTrack(std::shared_ptr<rtc::Track> track) {
        if (!track) return;
        const auto description = track->description();
        VerboseLog("[INFO]  remote_webrtc: negotiated track type=%s mid=%s\n",
                   description.type().c_str(),
                   description.mid().c_str());
        if (description.type() == "video") {
            ConfigureVideoTrack(std::move(track));
        } else if (description.type() == "audio") {
            ConfigureAudioTrack(std::move(track));
        } else {
            track->close();
        }
    }

    ~NativeWebRtcPeer() override {
        StopAudioPump();
        StopVideoPump();
        if (peer_) peer_->close();
    }

    WebRtcAnswer AcceptOffer(const std::string& sdp) override {
        try {
            {
                std::lock_guard<std::mutex> lock(mu_);
                description_ready_ = false;
                local_type_.clear();
                local_sdp_.clear();
                candidates_ = nlohmann::json::array();
                gathering_complete_ = false;
            }

            peer_->setRemoteDescription(rtc::Description(sdp, "offer"));
            peer_->setLocalDescription(rtc::Description::Type::Answer);

            // Two-phase wait so the answer turns around in tens of
            // milliseconds even when STUN is unreachable:
            //
            // 1. Hard wait (up to 5s) for `description_ready_` - without the
            //    local SDP there is no answer to return at all.
            // 2. Soft wait (a short tail window after SDP arrives) to let
            //    libdatachannel emit the synchronous host candidates so the
            //    initial answer's `candidates[]` is non-empty for browsers
            //    that don't act on trickle until the answer is applied.
            //
            // We intentionally do NOT wait on `gathering_complete_`. STUN
            // probing can hang for the full network timeout (~9s) when UDP
            // 3478 is black-holed (locked-down ISPs, Tailscale tailnets
            // without exit-node STUN reachability, etc), and any srflx /
            // relay candidate we miss here is still trickled to the
            // browser via `LocalIceCandidateHandler` as it arrives.
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::seconds(5), [this] {
                return description_ready_;
            });
            if (local_sdp_.empty()) {
                return WebRtcAnswer{.ok = false, .error = "timed out creating WebRTC answer"};
            }
            // Brief tail wait for the first batch of host candidates. They
            // are produced synchronously by libdatachannel right after
            // setLocalDescription, so 100ms is generous; we bail early as
            // soon as gathering finishes (LAN-only path).
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return gathering_complete_;
            });
            return WebRtcAnswer{
                .ok = true,
                .sdp = local_sdp_,
                .candidates = candidates_,
            };
        } catch (const std::exception& e) {
            return WebRtcAnswer{.ok = false, .error = e.what()};
        }
    }

    bool AddIceCandidate(const nlohmann::json& candidate, std::string* error) override {
        try {
            const std::string value = candidate.value("candidate", "");
            const std::string mid = candidate.value("sdpMid", candidate.value("mid", ""));
            if (value.empty()) return true;
            if (mid.empty()) {
                peer_->addRemoteCandidate(rtc::Candidate(value));
            } else {
                peer_->addRemoteCandidate(rtc::Candidate(value, mid));
            }
            return true;
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        }
    }

    void PushAudio(RemoteAudioChunk chunk) override {
        if (chunk.pcm.empty()) return;
        {
            std::lock_guard<std::mutex> lock(audio_mu_);
            audio_queue_.push_back(std::move(chunk));
            while (audio_queue_.size() > kMaxQueuedAudioChunks) audio_queue_.pop_front();
        }
        audio_cv_.notify_one();
    }

    void SetVideoBitrate(uint32_t bitrate_bps) override {
        bitrate_bps = std::max<uint32_t>(500'000, std::min<uint32_t>(20'000'000, bitrate_bps));
        video_bitrate_bps_ = bitrate_bps;
        force_keyframe_requested_ = true;
        // Bitrate is set at the start of every session and on every client
        // resize; the value also shows up in the next "encoder opened" line
        // (if encoding actually reconfigures), so default to verbose.
        VerboseLog("[INFO]  remote_webrtc: video bitrate target=%u bps\n", bitrate_bps);
    }

    void SetDataChannelHandler(DataChannelMessageHandler handler) override {
        std::lock_guard<std::mutex> lock(mu_);
        dc_handler_ = std::move(handler);
    }

    void SetLocalIceCandidateHandler(LocalIceCandidateHandler handler) override {
        // Snapshot any candidates already gathered before the embedder
        // installed the trickle handler (typical race: handler is
        // attached right after CreateWebRtcPeer but onLocalCandidate
        // fires from the libdatachannel worker as soon as
        // setLocalDescription is called). Replaying ensures the browser
        // ends up with the same candidate set regardless of timing.
        std::vector<nlohmann::json> backlog;
        {
            std::lock_guard<std::mutex> lock(mu_);
            local_ice_handler_ = handler;
            if (handler && !candidates_.empty()) {
                for (const auto& c : candidates_) backlog.push_back(c);
            }
        }
        for (auto& c : backlog) {
            try {
                handler(std::move(c));
            } catch (const std::exception& e) {
                std::fprintf(stdout,
                             "[ERROR] remote_webrtc: local ice handler threw on backlog: %s\n",
                             e.what());
                std::fflush(stdout);
            }
        }
    }

    void SetPeerClosedHandler(PeerClosedHandler handler) override {
        // Replay an already-dispatched terminal state so a handler
        // installed late (e.g. embedder swaps in its callback after
        // the peer has already crashed during AcceptOffer) still gets
        // a chance to clean up. The dispatched flag prevents
        // double-invocation if the underlying state machine fires
        // Failed -> Closed afterward.
        bool replay = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            peer_closed_handler_ = handler;
            if (handler && peer_closed_dispatched_) replay = true;
        }
        if (replay && handler) {
            try {
                handler("peer_closed");
            } catch (const std::exception& e) {
                std::fprintf(stdout,
                             "[ERROR] remote_webrtc: peer closed handler threw on replay: %s\n",
                             e.what());
                std::fflush(stdout);
            }
        }
    }

    void SetDataChannelOpenHandler(DataChannelOpenHandler handler) override {
        // Snapshot any already-open channels so a handler installed after
        // open() still gets a chance to seed initial state. Channels we
        // attached but haven't seen open yet will fire the handler from
        // their onOpen callback when they transition.
        std::vector<std::string> already_open_labels;
        {
            std::lock_guard<std::mutex> lock(mu_);
            dc_open_handler_ = handler;
            for (const auto& channel : data_channels_) {
                if (channel && channel->isOpen()) {
                    already_open_labels.push_back(channel->label());
                }
            }
        }
        if (!handler) return;
        for (const auto& label : already_open_labels) {
            try {
                handler(label);
            } catch (const std::exception& e) {
                std::fprintf(stdout,
                             "[ERROR] remote_webrtc: dc open handler threw on label=%s: %s\n",
                             label.c_str(),
                             e.what());
                std::fflush(stdout);
            }
        }
    }

    bool SendOnDataChannel(const std::string& label, const std::string& text) override {
        std::shared_ptr<rtc::DataChannel> target;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& channel : data_channels_) {
                if (channel && channel->label() == label) {
                    target = channel;
                    break;
                }
            }
        }
        if (!target || !target->isOpen()) return false;
        try {
            target->send(text);
            return true;
        } catch (const std::exception& e) {
            std::fprintf(stdout,
                         "[WARN]  remote_webrtc: send on dc=%s failed: %s\n",
                         label.c_str(),
                         e.what());
            std::fflush(stdout);
            return false;
        }
    }

private:
    void AttachDataChannel(std::shared_ptr<rtc::DataChannel> channel) {
        if (!channel) return;
        const std::string label = channel->label();
        // Two channels (input-fast + control) are attached and torn down on
        // every session, so this is 4 lines per reconnect — verbose only.
        VerboseLog("[INFO]  remote_webrtc: data channel attached label=%s\n",
                   label.c_str());
        std::weak_ptr<NativeWebRtcPeer> self_weak = weak_from_this();
        std::weak_ptr<rtc::DataChannel> channel_weak = channel;
        channel->onMessage(
            [self_weak, channel_weak, label](rtc::binary /*data*/) {
                if (!self_weak.lock() || channel_weak.expired()) return;
                std::fprintf(stdout,
                             "[WARN]  remote_webrtc: dropping binary frame on dc=%s "
                             "(only JSON text messages are supported)\n",
                             label.c_str());
                std::fflush(stdout);
            },
            [self_weak, label](std::string text) {
                if (auto self = self_weak.lock()) {
                    self->DispatchDataChannelText(label, std::move(text));
                }
            });
        channel->onOpen([self_weak, label] {
            if (auto self = self_weak.lock()) self->DispatchDataChannelOpen(label);
        });
        channel->onClosed([self_weak, label] {
            if (!self_weak.lock()) return;
            VerboseLog("[INFO]  remote_webrtc: data channel closed label=%s\n",
                       label.c_str());
        });
        // If the channel was already open by the time we wired onOpen (e.g.
        // libdatachannel may have moved it through Open during attach), fire
        // the dispatcher manually so the seed-state path still runs.
        const bool already_open = channel->isOpen();
        {
            std::lock_guard<std::mutex> lock(mu_);
            data_channels_.push_back(std::move(channel));
        }
        if (already_open) DispatchDataChannelOpen(label);
    }

    void DispatchDataChannelOpen(const std::string& label) {
        DataChannelOpenHandler handler;
        {
            std::lock_guard<std::mutex> lock(mu_);
            handler = dc_open_handler_;
        }
        if (!handler) return;
        try {
            handler(label);
        } catch (const std::exception& e) {
            std::fprintf(stdout,
                         "[ERROR] remote_webrtc: dc open handler threw on label=%s: %s\n",
                         label.c_str(),
                         e.what());
            std::fflush(stdout);
        }
    }

    void DispatchDataChannelText(const std::string& label, std::string text) {
        DataChannelMessageHandler handler;
        {
            std::lock_guard<std::mutex> lock(mu_);
            handler = dc_handler_;
        }
        if (!handler) return;
        nlohmann::json message;
        try {
            message = nlohmann::json::parse(text);
        } catch (const std::exception& e) {
            std::fprintf(stdout,
                         "[WARN]  remote_webrtc: bad JSON on dc=%s: %s\n",
                         label.c_str(),
                         e.what());
            std::fflush(stdout);
            return;
        }
        if (!message.is_object()) return;
        // Surface the channel label so clipboard/control vs input-fast can be
        // distinguished if the routing layer cares (e.g. for QoS metrics).
        message["channel"] = label;
        try {
            handler(message);
        } catch (const std::exception& e) {
            std::fprintf(stdout,
                         "[ERROR] remote_webrtc: dc handler threw on label=%s: %s\n",
                         label.c_str(),
                         e.what());
            std::fflush(stdout);
        }
    }

private:
    class VideoMetadataExtensionHandler final : public rtc::MediaHandler {
    public:
        VideoMetadataExtensionHandler(uint8_t color_space_id,
                                      uint8_t playout_delay_id)
            : color_space_id_(color_space_id),
              playout_delay_id_(playout_delay_id) {}

        void outgoing(rtc::message_vector& messages, const rtc::message_callback& /*send*/) override {
            if (color_space_id_ == 0 && playout_delay_id_ == 0) return;
            for (auto& message : messages) {
                if (!message || message->type != rtc::Message::Binary || message->size() < sizeof(rtc::RtpHeader)) {
                    continue;
                }
                auto* header = reinterpret_cast<rtc::RtpHeader*>(message->data());
                if (!header->marker()) continue;

                if (color_space_id_ != 0) {
                    const uint8_t range_chroma = static_cast<uint8_t>(1 << 4);
                    const uint8_t color_space[] = {
                        1,   // BT.709 primaries
                        13,  // IEC 61966-2-1 / sRGB transfer
                        1,   // BT.709 matrix
                        range_chroma,
                    };
                    AddRtpHeaderExtension(*message, color_space_id_, color_space, sizeof(color_space));
                }
                if (playout_delay_id_ != 0) {
                    // Per draft-ietf-avtext-rtp-hdrext-playout-delay: each
                    // value is 12 bits in 10ms units (so MIN/MAX delay range
                    // is 0..40950ms). We send min=max=10ms instead of 0
                    // because some receivers (notably WebKit/Safari) treat
                    // min=0 as "render nothing" and the <video> element
                    // stays black. 10ms is small enough to behave like
                    // "render ASAP" everywhere.
                    constexpr uint16_t kDelay10Ms = 1;
                    const uint8_t playout_delay[] = {
                        static_cast<uint8_t>((kDelay10Ms >> 4) & 0xff),
                        static_cast<uint8_t>(((kDelay10Ms & 0x0f) << 4) | ((kDelay10Ms >> 8) & 0x0f)),
                        static_cast<uint8_t>(kDelay10Ms & 0xff),
                    };
                    AddRtpHeaderExtension(*message, playout_delay_id_, playout_delay, sizeof(playout_delay));
                }
            }
        }

    private:
        static void AddRtpHeaderExtension(rtc::Message& message,
                                          uint8_t extension_id,
                                          const uint8_t* value,
                                          size_t value_size) {
            if (extension_id == 0 || !value || value_size == 0 || value_size > 255) return;
            auto* header = reinterpret_cast<rtc::RtpHeader*>(message.data());
            const size_t rtp_header_size = header->getSize();
            const bool has_extension = header->extension();
            const size_t old_extension_size = header->getExtensionHeaderSize();
            const bool use_two_byte = has_extension &&
                header->getExtensionHeader() &&
                header->getExtensionHeader()->profileSpecificId() == 0x1000;
            if (!use_two_byte && extension_id > 14) return;

            const size_t old_body_size = old_extension_size >= sizeof(rtc::RtpExtensionHeader)
                ? old_extension_size - sizeof(rtc::RtpExtensionHeader)
                : 0;
            const size_t element_size = use_two_byte ? (2 + value_size) : (1 + value_size);
            const size_t new_body_size = (old_body_size + element_size + 3) & ~size_t{3};
            const size_t new_extension_size = sizeof(rtc::RtpExtensionHeader) + new_body_size;
            const size_t insert_at = rtp_header_size + old_extension_size;
            const size_t insert_size = has_extension
                ? new_extension_size - old_extension_size
                : new_extension_size;
            message.insert(message.begin() + static_cast<std::ptrdiff_t>(insert_at), insert_size, std::byte{0});

            header = reinterpret_cast<rtc::RtpHeader*>(message.data());
            header->setExtension(true);
            auto* extension = header->getExtensionHeader();
            extension->setProfileSpecificId(use_two_byte ? 0x1000 : 0xbede);
            extension->setHeaderLength(static_cast<uint16_t>(new_body_size / 4));
            auto* body = reinterpret_cast<uint8_t*>(extension->getBody());
            if (!has_extension) std::memset(body, 0, new_body_size);
            if (use_two_byte) {
                body[old_body_size] = extension_id;
                body[old_body_size + 1] = static_cast<uint8_t>(value_size);
                std::memcpy(body + old_body_size + 2, value, value_size);
            } else {
                if (value_size > 16) return;
                body[old_body_size] = static_cast<uint8_t>((extension_id << 4) | (value_size - 1));
                std::memcpy(body + old_body_size + 1, value, value_size);
            }
        }

        uint8_t color_space_id_ = 0;
        uint8_t playout_delay_id_ = 0;
    };

    void ConfigureVideoTrack(std::shared_ptr<rtc::Track> track) {
        video_track_ = std::move(track);
        auto video = video_track_->description();
        video.addSSRC(kVideoSsrc, "tenbox-video", "tenbox-stream", "tenbox-video");
        const auto selection = ChooseH264Payload(video, preferred_video_format_).value_or(
            H264PayloadSelection{kFallbackVideoPayloadType, H264Profile::kConstrainedBaseline});
        const uint8_t payload_type = selection.payload_type;
        negotiated_h264_profile_.store(selection.profile, std::memory_order_relaxed);
        const uint8_t color_space_id = FindExtMapId(video, "color-space").value_or(0);
        const uint8_t playout_delay_id = FindExtMapId(video, "playout-delay").value_or(0);
        video_track_->setDescription(std::move(video));
        auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
            kVideoSsrc, "tenbox-video", payload_type, rtc::H264RtpPacketizer::ClockRate);
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::Length, rtp_config);
        if (color_space_id != 0 || playout_delay_id != 0) {
            packetizer->addToChain(std::make_shared<VideoMetadataExtensionHandler>(
                color_space_id, playout_delay_id));
        }
        // Intentionally do NOT add an RtcpSrReporter for video. Without RTCP
        // Sender Reports the receiver cannot map our RTP timestamps to NTP
        // wall-clock, so Chrome's RtpStreamsSynchronizer (lip-sync) has no
        // calibration for video and silently disables A/V sync. This stops
        // Chrome from inflating the video jitter buffer to "wait for audio"
        // when the audio path has any extra delay. Audio still has its SR,
        // so audio playback is unaffected. Side effect: per-RTP-stream RTT
        // estimation via LSR/DLSR is gone for video; ICE candidate-pair RTT
        // (browser getStats) is unaffected.
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        std::weak_ptr<NativeWebRtcPeer> weak = weak_from_this();
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([weak]() {
            auto self = weak.lock();
            if (!self) return;
            self->force_keyframe_requested_ = true;
            // Receivers fire PLI on every key frame loss / freeze recovery,
            // sometimes several times per second. Default to verbose; even
            // verbose users get rate-limited info aggregated below by the
            // encoder thread (request count is reflected via force_keyframe).
            VerboseLog("[INFO]  remote_webrtc: keyframe requested by receiver\n");
        }));
        video_track_->setMediaHandler(packetizer);
        VerboseLog("[INFO]  remote_webrtc: video payload type=%u mid=%s color_ext=%u playout_ext=%u\n",
                   payload_type,
                   video_track_->mid().c_str(),
                   color_space_id,
                   playout_delay_id);
        video_track_->onOpen([weak]() {
            auto self = weak.lock();
            if (!self) return;
            VerboseLog("[INFO]  remote_webrtc: video track opened\n");
            self->StartVideoPump();
        });
    }

    void ConfigureAudioTrack(std::shared_ptr<rtc::Track> track) {
        audio_track_ = std::move(track);
        auto audio = audio_track_->description();
        audio.addSSRC(kAudioSsrc, "tenbox-audio", "tenbox-stream", "tenbox-audio");
        const uint8_t payload_type = ChooseOpusPayloadType(audio).value_or(kFallbackAudioPayloadType);
        MarkOpusDtx(audio, payload_type);
        audio_track_->setDescription(std::move(audio));
        auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
            kAudioSsrc, "tenbox-audio", payload_type, rtc::OpusRtpPacketizer::DefaultClockRate);
        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp_config);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_config));
        audio_track_->setMediaHandler(packetizer);
        VerboseLog("[INFO]  remote_webrtc: audio payload type=%u mid=%s\n",
                   payload_type,
                   audio_track_->mid().c_str());
        std::weak_ptr<NativeWebRtcPeer> weak = weak_from_this();
        audio_track_->onOpen([weak]() {
            auto self = weak.lock();
            if (!self) return;
            VerboseLog("[INFO]  remote_webrtc: audio track opened\n");
            self->StartAudioPump();
        });
    }

    struct H264PayloadSelection {
        uint8_t payload_type;
        H264Profile profile;
        bool high444 = false;
        bool constrained_baseline = false;
    };

    static const char* H264ProfileLogName(H264Profile profile) {
        switch (profile) {
        case H264Profile::kHigh:
            return "high";
        case H264Profile::kMain:
            return "main";
        case H264Profile::kConstrainedBaseline:
        default:
            return "constrained-baseline";
        }
    }

    static const char* PixelFormatLogName(PixelFormat format) {
        switch (format) {
        case PixelFormat::kYuv444p:
            return "yuv444p";
        case PixelFormat::kYuv420p:
            return "yuv420p";
        case PixelFormat::kRgba:
            return "rgba";
        case PixelFormat::kBgra:
            return "bgra";
        default:
            return "unknown";
        }
    }

    static size_t ChromaPlaneHeight(PixelFormat format, uint32_t height) {
        return format == PixelFormat::kYuv444p
            ? static_cast<size_t>(height)
            : static_cast<size_t>((height + 1) / 2);
    }

    // Walk the offer's H.264 payload types and parse each fmtp's
    // `profile-level-id` (6 hex digits: profile_idc | constraint_flags |
    // level_idc). Prefer Main over High because Linux Chrome's NVIDIA/VAAPI
    // hardware decode path rejects our NVENC High-profile WebRTC stream.
    static std::optional<H264PayloadSelection> ChooseH264Payload(
        const rtc::Description::Media& media,
        PixelFormat preferred_video_format) {
        std::optional<H264PayloadSelection> baseline_choice;
        std::optional<H264PayloadSelection> constrained_baseline_choice;
        std::optional<H264PayloadSelection> main_choice;
        std::optional<H264PayloadSelection> high_choice;
        std::optional<H264PayloadSelection> high444_choice;
        auto from_hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
        };
        for (int payload_type : media.payloadTypes()) {
            if (payload_type < 0 || payload_type > 127) continue;
            const auto* rtp_map = media.rtpMap(payload_type);
            if (!rtp_map) continue;
            if (rtp_map->format != "H264" && rtp_map->format != "h264") continue;

            H264Profile profile = H264Profile::kConstrainedBaseline;
            bool high444 = false;
            bool constrained_baseline = false;
            bool has_profile_id = false;
            for (const auto& fmtp : rtp_map->fmtps) {
                const std::string_view sv(fmtp);
                const std::string_view key("profile-level-id=");
                const auto pos = sv.find(key);
                if (pos == std::string_view::npos) continue;
                const auto start = pos + key.size();
                if (start + 4 > sv.size()) continue;
                const int hi = from_hex(sv[start]);
                const int lo = from_hex(sv[start + 1]);
                const int flags_hi = from_hex(sv[start + 2]);
                const int flags_lo = from_hex(sv[start + 3]);
                if (hi < 0 || lo < 0 || flags_hi < 0 || flags_lo < 0) continue;
                const uint8_t profile_idc =
                    static_cast<uint8_t>((hi << 4) | lo);
                const uint8_t constraint_flags =
                    static_cast<uint8_t>((flags_hi << 4) | flags_lo);
                has_profile_id = true;
                if (profile_idc == 0x64 || profile_idc == 0xF4) {
                    profile = H264Profile::kHigh;
                    high444 = profile_idc == 0xF4;
                } else if (profile_idc == 0x4D) {
                    profile = H264Profile::kMain;
                } else {
                    // 0x42 (baseline), 0x58 (extended), or anything we don't
                    // explicitly support: encode as constrained-baseline for
                    // maximum compatibility.
                    profile = H264Profile::kConstrainedBaseline;
                    constrained_baseline = profile_idc == 0x42 && (constraint_flags & 0x40) != 0;
                }
                break;
            }
            (void)has_profile_id;

            const H264PayloadSelection sel{
                static_cast<uint8_t>(payload_type), profile, high444, constrained_baseline};
            if (high444) {
                if (!high444_choice) high444_choice = sel;
            } else if (profile == H264Profile::kHigh) {
                if (!high_choice) high_choice = sel;
            } else if (profile == H264Profile::kMain) {
                if (!main_choice) main_choice = sel;
            } else if (constrained_baseline) {
                if (!constrained_baseline_choice) constrained_baseline_choice = sel;
            } else {
                if (!baseline_choice) baseline_choice = sel;
            }
        }
        if (preferred_video_format == PixelFormat::kYuv444p && high444_choice) {
            return high444_choice;
        }
        if (PreferConstrainedBaselineH264()) {
            if (constrained_baseline_choice) return constrained_baseline_choice;
            if (baseline_choice) return baseline_choice;
        }
        if (main_choice) return main_choice;
        if (high_choice) return high_choice;
        return baseline_choice;
    }

    static std::optional<uint8_t> ChooseOpusPayloadType(const rtc::Description::Media& media) {
        for (int payload_type : media.payloadTypes()) {
            const auto* rtp_map = media.rtpMap(payload_type);
            if (!rtp_map) continue;
            if (rtp_map->format == "opus" || rtp_map->format == "OPUS") {
                if (payload_type >= 0 && payload_type <= 127) {
                    return static_cast<uint8_t>(payload_type);
                }
            }
        }
        return std::nullopt;
    }

    static void MarkOpusDtx(rtc::Description::Media& media, uint8_t payload_type) {
        if (!media.hasPayloadType(payload_type)) return;
        auto* rtp_map = media.rtpMap(payload_type);
        if (!rtp_map) return;
        rtp_map->removeParameter("usedtx");
        rtp_map->removeParameter("maxaveragebitrate");
        if (rtp_map->fmtps.empty()) {
            rtp_map->fmtps.emplace_back("usedtx=1;maxaveragebitrate=64000");
            return;
        }
        rtp_map->fmtps.front() += ";usedtx=1;maxaveragebitrate=64000";
    }

    static std::optional<uint8_t> FindExtMapId(rtc::Description::Media& media, std::string_view needle) {
        for (int id : media.extIds()) {
            const auto* ext = media.extMap(id);
            if (!ext) continue;
            if (ext->uri.find(needle) == std::string::npos) continue;
            if (id > 0 && id <= 255) return static_cast<uint8_t>(id);
        }
        return std::nullopt;
    }

    void StartVideoPump() {
        bool expected = false;
        if (!video_running_.compare_exchange_strong(expected, true)) return;
        video_thread_ = std::thread([this]() {
            VideoPumpMain();
        });
    }

    void StopVideoPump() {
        video_running_ = false;
        if (video_thread_.joinable()) video_thread_.join();
    }

    void StartAudioPump() {
        bool expected = false;
        if (!audio_running_.compare_exchange_strong(expected, true)) return;
        audio_thread_ = std::thread([this]() {
            AudioPumpMain();
        });
    }

    void StopAudioPump() {
        audio_running_ = false;
        audio_cv_.notify_all();
        if (audio_thread_.joinable()) audio_thread_.join();
    }

    void AudioPumpMain() {
        pthread_setname_np(pthread_self(), "webrtc-audio");
        constexpr uint32_t kSampleRate = 48000;
        constexpr uint32_t kFrameMs = 20;
        constexpr size_t kFrameSamplesPerChannel = kSampleRate * kFrameMs / 1000;

        OpusAudioEncoder encoder;
        bool encoder_open = false;
        uint32_t channels = 0;
        // Sliding-window staging buffer for PCM samples awaiting a 20ms Opus
        // frame. Consumers advance `pcm_read_offset` instead of doing
        // `vector::erase(begin, ...)` on every frame (which would memmove the
        // remainder of the buffer). The tail is compacted lazily once the
        // head waste exceeds half of the buffer.
        std::vector<int16_t> pcm_buffer;
        size_t pcm_read_offset = 0;
        const auto pcm_available = [&]() -> size_t {
            return pcm_buffer.size() - pcm_read_offset;
        };
        const auto pcm_advance = [&](size_t consumed) {
            pcm_read_offset += consumed;
            if (pcm_read_offset >= pcm_buffer.size()) {
                pcm_buffer.clear();
                pcm_read_offset = 0;
            } else if (pcm_read_offset >= pcm_buffer.size() / 2 &&
                       pcm_read_offset >= 4096) {
                pcm_buffer.erase(pcm_buffer.begin(),
                                 pcm_buffer.begin() + static_cast<std::ptrdiff_t>(pcm_read_offset));
                pcm_read_offset = 0;
            }
        };
        int64_t pts_us = 0;
        uint64_t sent_frames = 0;
        uint64_t dropped_frames = 0;

        while (audio_running_) {
            RemoteAudioChunk chunk;
            {
                std::unique_lock<std::mutex> lock(audio_mu_);
                audio_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !audio_running_ || !audio_queue_.empty();
                });
                if (!audio_running_) break;
                if (audio_queue_.empty()) continue;
                chunk = std::move(audio_queue_.front());
                audio_queue_.pop_front();
            }

            if (chunk.sample_rate != kSampleRate || chunk.channels == 0 || chunk.channels > 2) {
                if (sent_frames == 0) {
                    std::fprintf(stdout,
                                 "[WARN]  remote_webrtc: dropping unsupported audio format %uHz/%uch\n",
                                 chunk.sample_rate,
                                 chunk.channels);
                    std::fflush(stdout);
                }
                continue;
            }
            if (!encoder_open || channels != chunk.channels) {
                std::string error;
                encoder_open = encoder.Open(chunk.sample_rate, chunk.channels, &error);
                channels = chunk.channels;
                pcm_buffer.clear();
                pcm_read_offset = 0;
                pts_us = 0;
                if (!encoder_open) {
                    std::fprintf(stdout, "[WARN]  remote_webrtc: opus open failed: %s\n", error.c_str());
                    std::fflush(stdout);
                    continue;
                }
                // Opus settings (48k/2ch) basically never change after the
                // first session, so the line is informational once and pure
                // noise on every reconnect.
                VerboseLog("[INFO]  remote_webrtc: opus encoder opened %uHz/%uch\n",
                           chunk.sample_rate,
                           chunk.channels);
            }

            pcm_buffer.insert(pcm_buffer.end(), chunk.pcm.begin(), chunk.pcm.end());
            const size_t frame_samples = kFrameSamplesPerChannel * channels;
            const size_t max_buffer_samples = frame_samples * kMaxBufferedAudioFrames;
            while (pcm_available() > max_buffer_samples) {
                pcm_advance(frame_samples);
                pts_us += static_cast<int64_t>(kFrameMs) * 1000;
                ++dropped_frames;
            }
            while (pcm_available() >= frame_samples && audio_running_) {
                if (audio_track_ && audio_track_->bufferedAmount() > kMaxAudioBufferedBytes) {
                    pcm_advance(frame_samples);
                    pts_us += static_cast<int64_t>(kFrameMs) * 1000;
                    ++dropped_frames;
                    continue;
                }

                AudioChunk input;
                input.sample_rate = kSampleRate;
                input.channels = channels;
                input.samples = std::span<const int16_t>(
                    pcm_buffer.data() + pcm_read_offset, frame_samples);
                input.pts_us = pts_us;

                EncodedAudioFrame encoded;
                std::string error;
                if (!encoder.Encode(input, &encoded, &error) || encoded.data.empty()) {
                    if (!error.empty()) {
                        std::fprintf(stdout, "[WARN]  remote_webrtc: opus encode failed: %s\n", error.c_str());
                        std::fflush(stdout);
                    }
                    pcm_advance(frame_samples);
                    pts_us += static_cast<int64_t>(kFrameMs) * 1000;
                    continue;
                }
                if (!audio_track_) break;

                const auto* bytes = reinterpret_cast<const std::byte*>(encoded.data.data());
                rtc::binary sample(bytes, bytes + encoded.data.size());
                try {
                    audio_track_->sendFrame(sample, std::chrono::duration<double, std::micro>(encoded.pts_us));
                } catch (const std::exception& e) {
                    if (sent_frames == 0) {
                        std::fprintf(stdout, "[WARN]  remote_webrtc: audio sendFrame failed: %s\n", e.what());
                        std::fflush(stdout);
                    }
                    break;
                }

                pcm_advance(frame_samples);
                pts_us += static_cast<int64_t>(kFrameMs) * 1000;
                ++sent_frames;
            }
        }
    }

    void VideoPumpMain() {
        pthread_setname_np(pthread_self(), "webrtc-video");
        FfmpegH264VideoEncoder encoder;
        VideoEncoderConfig config;
        bool encoder_open = false;
        // The encoder's persistent input buffer is empty after Open(); the next
        // drain must request a full-frame slice from the producer to seed it.
        bool needs_full_seed = true;
        uint64_t sent_frames = 0;
        bool waiting_logged = false;
        // Track what we last logged for the "encoder opened" line so that
        // pure bitrate adjustments (which retrigger encoder.Open() but are
        // not interesting on a per-event basis) do not spam the log; a real
        // INFO line only fires when the resolution / pixel format / encoder
        // backend / H.264 profile actually changes.
        uint32_t last_logged_width = 0;
        uint32_t last_logged_height = 0;
        PixelFormat last_logged_input_format = PixelFormat::kYuv420p;
        H264Profile last_logged_profile = H264Profile::kConstrainedBaseline;
        std::string last_logged_encoder_name;
        const auto start = std::chrono::steady_clock::now();
        const auto base_frame_interval = std::chrono::microseconds(16'667);
        const auto idle_frame_interval = std::chrono::milliseconds(500);
        const auto recovery_grace_period = std::chrono::milliseconds(2000);
        std::optional<std::chrono::steady_clock::time_point> last_slice_time;
        // Fixed upper frame-rate cap. Bitrate pressure is handled by the encoder,
        // but we still avoid sending faster than the negotiated 60fps cadence.
        auto next_encode_time = start;

        while (video_running_) {
            const auto pacing_now = std::chrono::steady_clock::now();
            if (pacing_now < next_encode_time) {
                std::this_thread::sleep_until(next_encode_time);
                if (!video_running_) break;
            }

            // After motion stops, keep re-encoding the settled frame briefly so
            // low-bitrate streams can recover detail before falling to idle.
            const auto read_now = std::chrono::steady_clock::now();
            const bool in_recovery_window =
                last_slice_time.has_value() &&
                read_now - *last_slice_time < recovery_grace_period;
            const auto frame_wait_timeout =
                in_recovery_window ? std::chrono::milliseconds(0) : idle_frame_interval;

            // Block on the producer's CV until new slices arrive (or timeout).
            // This replaces the old sleep-then-poll pattern so a fresh slice
            // wakes the encoder thread immediately, while the recovery window
            // avoids dropping straight to 2fps after recent motion.
            RemoteVideoFrame remote_frame;
            const bool got_slices = frame_reader_ &&
                frame_reader_(&remote_frame, needs_full_seed, frame_wait_timeout) &&
                !remote_frame.slices.empty();
            if (got_slices) {
                last_slice_time = std::chrono::steady_clock::now();
            }

            // Without a seeded encoder we cannot produce a heartbeat frame;
            // wait for the producer to deliver the first full-frame slice.
            if (!got_slices && (!encoder_open || !encoder.HasFullSeed())) {
                if (!waiting_logged) {
                    // First-time-only when we cannot start because the
                    // producer hasn't pushed any framebuffer yet. After the
                    // session goes live this should not recur, so leaving
                    // it at INFO is safe.
                    std::fprintf(stdout, "[INFO]  remote_webrtc: waiting for framebuffer\n");
                    std::fflush(stdout);
                    waiting_logged = true;
                }
                continue;
            }
            waiting_logged = false;

            const bool force_keyframe = force_keyframe_requested_.exchange(false);
            const auto now = std::chrono::steady_clock::now();

            if (video_track_ && video_track_->bufferedAmount() > kMaxVideoBufferedBytes) {
                if (force_keyframe) force_keyframe_requested_ = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            const uint32_t target_bitrate_bps = video_bitrate_bps_.load();
            const H264Profile target_profile =
                negotiated_h264_profile_.load(std::memory_order_relaxed);
            if (got_slices &&
                (!encoder_open ||
                 config.width != remote_frame.width ||
                 config.height != remote_frame.height ||
                 config.bitrate_bps != target_bitrate_bps ||
                 config.input_format != remote_frame.format ||
                 config.h264_profile != target_profile)) {
                config.width = remote_frame.width;
                config.height = remote_frame.height;
                config.input_format = remote_frame.format;
                config.codec = VideoCodec::kH264;
                config.framerate = 60;
                config.bitrate_bps = target_bitrate_bps;
                config.h264_profile = target_profile;
                std::string error;
                encoder_open = encoder.Open(config, &error);
                if (!encoder_open) {
                    std::fprintf(stdout, "[WARN]  remote_webrtc: encoder open failed: %s\n", error.c_str());
                    std::fflush(stdout);
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                const std::string encoder_name = encoder.SelectedEncoderName();
                // Promote the "encoder opened" line to INFO only when one
                // of the user-visible knobs (resolution / pixel format /
                // backend / H.264 profile) actually changed since the last
                // print. Bitrate-only retunes still re-Open() the encoder
                // and are interesting at debug level, but logging them at
                // INFO would flood the journal whenever the bandwidth
                // controller adapts.
                const bool material_change =
                    remote_frame.width != last_logged_width ||
                    remote_frame.height != last_logged_height ||
                    config.input_format != last_logged_input_format ||
                    config.h264_profile != last_logged_profile ||
                    encoder_name != last_logged_encoder_name;
                // The newly opened encoder has zeroed input planes; the slices
                // we just drained may be partial, so request a full-frame
                // reseed on the next iteration before encoding.
                if (!encoder.HasFullSeed() &&
                    !(remote_frame.slices.size() == 1 &&
                      remote_frame.slices.front().x == 0 &&
                      remote_frame.slices.front().y == 0 &&
                      remote_frame.slices.front().width == remote_frame.width &&
                      remote_frame.slices.front().height == remote_frame.height)) {
                    needs_full_seed = true;
                    if (material_change) {
                        std::fprintf(stdout,
                                     "[INFO]  remote_webrtc: encoder opened %ux%u bitrate=%u profile=%s format=%s encoder=%s (awaiting full seed)\n",
                                     remote_frame.width,
                                     remote_frame.height,
                                     config.bitrate_bps,
                                     H264ProfileLogName(config.h264_profile),
                                     PixelFormatLogName(config.input_format),
                                     encoder_name.c_str());
                        std::fflush(stdout);
                    } else {
                        VerboseLog("[INFO]  remote_webrtc: encoder reconfigured bitrate=%u (awaiting full seed)\n",
                                   config.bitrate_bps);
                    }
                    last_logged_width = remote_frame.width;
                    last_logged_height = remote_frame.height;
                    last_logged_input_format = config.input_format;
                    last_logged_profile = config.h264_profile;
                    last_logged_encoder_name = encoder_name;
                    continue;
                }
                if (material_change) {
                    std::fprintf(stdout,
                                 "[INFO]  remote_webrtc: encoder opened %ux%u bitrate=%u profile=%s format=%s encoder=%s\n",
                                 remote_frame.width,
                                 remote_frame.height,
                                 config.bitrate_bps,
                                 H264ProfileLogName(config.h264_profile),
                                 PixelFormatLogName(config.input_format),
                                 encoder_name.c_str());
                    std::fflush(stdout);
                } else {
                    VerboseLog("[INFO]  remote_webrtc: encoder reconfigured bitrate=%u\n",
                               config.bitrate_bps);
                }
                last_logged_width = remote_frame.width;
                last_logged_height = remote_frame.height;
                last_logged_input_format = config.input_format;
                last_logged_profile = config.h264_profile;
                last_logged_encoder_name = encoder_name;
            }

            // Apply every accumulated YUV slice to the encoder's persistent
            // input frame, mirroring sweet's DrawSlices step. When no slices
            // arrived this round we leave the persistent input untouched and
            // re-encode it as a heartbeat (faster during recovery, then 2fps idle).
            uint32_t total_pixels = 0;
            uint32_t slice_count = 0;
            if (got_slices) {
                std::string apply_error;
                bool apply_ok = true;
                for (const auto& s : remote_frame.slices) {
                    VideoSlice vs;
                    vs.x = s.x;
                    vs.y = s.y;
                    vs.width = s.width;
                    vs.height = s.height;
                    const size_t y_size = static_cast<size_t>(s.strides[0]) * s.height;
                    const size_t uv_h = ChromaPlaneHeight(remote_frame.format, s.height);
                    const size_t uv_size = static_cast<size_t>(s.strides[1]) * uv_h;
                    vs.planes[0] = s.data.data();
                    vs.planes[1] = s.data.data() + y_size;
                    vs.planes[2] = s.data.data() + y_size + uv_size;
                    vs.strides[0] = s.strides[0];
                    vs.strides[1] = s.strides[1];
                    vs.strides[2] = s.strides[2];
                    if (!encoder.ApplySlice(vs, &apply_error)) {
                        apply_ok = false;
                        break;
                    }
                    total_pixels += s.width * s.height;
                    ++slice_count;
                }
                if (!apply_ok) {
                    std::fprintf(stdout, "[WARN]  remote_webrtc: apply slice failed: %s\n", apply_error.c_str());
                    std::fflush(stdout);
                    needs_full_seed = true;
                    continue;
                }
                if (!encoder.HasFullSeed()) {
                    // Slices applied but the encoder still lacks a full
                    // reference; ask for a reseed and retry next round.
                    needs_full_seed = true;
                    continue;
                }
                needs_full_seed = false;
            }

            if (force_keyframe) {
                encoder.RequestKeyframe();
            }
            const auto pts_us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();

            EncodedVideoFrame encoded;
            std::string error;
            if (!encoder.EncodeFrame(pts_us, &encoded, &error) || encoded.data.empty()) {
                if (!error.empty()) {
                    std::fprintf(stdout, "[WARN]  remote_webrtc: encode failed: %s\n", error.c_str());
                    std::fflush(stdout);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (!video_track_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // The FFmpeg encoder wrapper returns AVCC (length-prefixed) NALUs
            // that the H264RtpPacketizer can consume verbatim. We still
            // allocate a fresh rtc::binary because libdatachannel takes
            // ownership of the buffer for the RTP send.
            const auto* video_bytes = reinterpret_cast<const std::byte*>(encoded.data.data());
            rtc::binary sample(video_bytes, video_bytes + encoded.data.size());
            if (sample.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            try {
                video_track_->sendFrame(sample, std::chrono::duration<double, std::micro>(encoded.pts_us));
            } catch (const std::exception& e) {
                if (sent_frames == 0) {
                    std::fprintf(stdout, "[WARN]  remote_webrtc: sendFrame failed: %s\n", e.what());
                    std::fflush(stdout);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            next_encode_time = now + base_frame_interval;
            ++sent_frames;
        }
    }

    static constexpr uint8_t kFallbackVideoPayloadType = 102;
    static constexpr uint8_t kFallbackAudioPayloadType = 111;
    static constexpr rtc::SSRC kVideoSsrc = 0x54424f58;
    static constexpr rtc::SSRC kAudioSsrc = 0x54424f41;
    static constexpr size_t kMaxQueuedAudioChunks = 100;
    static constexpr size_t kMaxVideoBufferedBytes = 0;
    static constexpr size_t kMaxBufferedAudioFrames = 5;
    static constexpr size_t kMaxAudioBufferedBytes = 128 * 1024;

    RemoteFrameReader frame_reader_;
    std::shared_ptr<rtc::PeerConnection> peer_;
    // Browser may open one or more channels (input-fast, control, ...). The
    // peer has no business inspecting the labels - it just forwards every
    // text/binary message decoded as JSON to `dc_handler_` and lets
    // `cloud_tunnel` do the routing.
    std::vector<std::shared_ptr<rtc::DataChannel>> data_channels_;
    DataChannelMessageHandler dc_handler_;
    DataChannelOpenHandler dc_open_handler_;
    LocalIceCandidateHandler local_ice_handler_;
    PeerClosedHandler peer_closed_handler_;
    bool peer_closed_dispatched_ = false;
    std::shared_ptr<rtc::Track> video_track_;
    std::shared_ptr<rtc::Track> audio_track_;
    std::thread video_thread_;
    std::thread audio_thread_;
    std::atomic<bool> video_running_{false};
    std::atomic<bool> audio_running_{false};
    std::atomic<bool> force_keyframe_requested_{false};
    std::atomic<uint32_t> video_bitrate_bps_{4'000'000};
    std::atomic<H264Profile> negotiated_h264_profile_{H264Profile::kConstrainedBaseline};
    PixelFormat preferred_video_format_ = PixelFormat::kYuv420p;
    std::mutex audio_mu_;
    std::condition_variable audio_cv_;
    std::deque<RemoteAudioChunk> audio_queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool description_ready_ = false;
    bool gathering_complete_ = false;
    std::string local_type_;
    std::string local_sdp_;
    nlohmann::json candidates_ = nlohmann::json::array();
};

std::shared_ptr<WebRtcPeer> CreateWebRtcPeer(
    RemoteFrameReader frame_reader,
    PixelFormat preferred_video_format) {
    auto peer = std::make_shared<NativeWebRtcPeer>(
        std::move(frame_reader),
        preferred_video_format);
    peer->InstallCallbacks();
    return peer;
}

bool NativeWebRtcAvailable() {
    return true;
}

std::vector<IceServerSpec> ResolvedIceServers() {
    return ConfiguredIceServers();
}

}  // namespace tenbox::daemon
