#pragma once

#include "daemon/kvm_doctor.h"
#include "daemon/remote_webrtc.h"
#include "daemon/resource_monitor.h"
#include "daemon/vm_store.h"
#include "ipc/protocol_v1.h"
#include "ipc/shared_framebuffer.h"
#include "ipc/unix_socket.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tenbox::daemon {

class RuntimeManager {
public:
    RuntimeManager(DaemonConfig config, VmStore& store);
    ~RuntimeManager();

    bool StartVm(const std::string& vm_id, std::string* error);
    bool StopVm(const std::string& vm_id, std::string* error);
    // Request an ACPI/QGA-driven graceful guest reboot. Sends the
    // `runtime.command` request with `command=reboot`; the runtime falls
    // back to a hard stop if QGA is not connected. Caller is expected to
    // gate on `guest_agent_connected`.
    bool RebootVm(const std::string& vm_id, std::string* error);
    // Request a guest-initiated shutdown (QGA `guest-shutdown`/QMP
    // `system_powerdown`). Same gating contract as RebootVm.
    bool ShutdownVm(const std::string& vm_id, std::string* error);
    bool AttachConsole(const std::string& vm_id, ipc::UnixSocketConnection* client, std::string* error);
    // Attach a follower for runtime stdout/stderr lines. Mirrors AttachConsole:
    // takes ownership of the underlying socket, pushes one ACK, and then
    // streams `vm.logs.append` JSON events line-by-line until the client
    // disconnects or the VM stops. Used by `tenbox vm logs -f`.
    bool AttachLogFollower(const std::string& vm_id, ipc::UnixSocketConnection* client, std::string* error);
    bool SendConsoleInput(const std::string& vm_id, const std::vector<uint8_t>& bytes);
    bool SendKeyEvent(const std::string& vm_id, uint32_t key_code, bool pressed);
    bool SendPointerEvent(const std::string& vm_id, int x, int y, uint32_t buttons);
    bool SendWheelEvent(const std::string& vm_id, int delta);
    bool SetDisplaySize(const std::string& vm_id, uint32_t width, uint32_t height);
    bool SetRemoteVideoPixelFormat(const std::string& vm_id, PixelFormat format);

    // Push the current spec's forward set to a running runtime via the
    // `runtime.update_network` control message. Reads the latest spec from
    // `store_` so callers can simply persist via VmStore::UpdateSpec and then
    // call this; the runtime applies the new forwards in-place without a
    // restart. Covers BOTH directions: Host->Guest (`host_forwards`,
    // qemu hostfwd) and Guest->Host (`guest_forwards`, qemu guestfwd) are
    // sent in the same message. Returns false if the VM is not running
    // (caller should treat as "queued for next start" since the persisted
    // spec already reflects the change).
    bool ApplyForwards(const std::string& vm_id);

    // Same idea for shared folders. Sends `runtime.update_shared_folders`;
    // the runtime diffs against the current virtiofs mount set and adds /
    // removes mounts as needed.
    bool ApplySharedFolders(const std::string& vm_id);

    // Toggle the live virtio-net link state on a running VM. Equivalent to
    // QEMU's `set_link <netdev> on/off` — keeps the device attached and the
    // forward set intact, so when the user re-enables it the VM regains
    // connectivity without needing to renegotiate DHCP from scratch (slirp
    // re-arms the lease as soon as the link comes back up). Returns false
    // if the VM isn't running; the persisted spec is authoritative so the
    // next start will pick up the change either way.
    bool ApplyNetLink(const std::string& vm_id, bool up);
    // Drain pending YUV slices for this VM into `frame`. When `need_full_frame`
    // is true, the producer regenerates a single full-frame slice from the
    // current shared framebuffer state and discards any partial slices.
    // When `wait_timeout` is non-zero and no slices are pending, this blocks
    // on the per-session condition variable until either a producer signals new
    // slices, the timeout expires, or `need_full_frame` lets us synthesize one
    // immediately from the shared framebuffer.
    bool ReadRemoteFrame(const std::string& vm_id,
                         RemoteVideoFrame* frame,
                         bool need_full_frame,
                         std::chrono::milliseconds wait_timeout = std::chrono::milliseconds(0));
    nlohmann::json RemoteRuntimeSnapshot(const std::string& vm_id) const;
    // Returns the most recently observed cursor JSON for `vm_id`, or an empty
    // object if no cursor event has been seen yet (or the VM is gone). Used
    // by the cloud tunnel to seed a freshly opened control DataChannel,
    // since virtio_gpu's source-side dedup means we cannot rely on the next
    // MOVE_CURSOR to deliver state to a late-attaching subscriber.
    nlohmann::json CursorSnapshot(const std::string& vm_id) const;
    using CursorCallback = std::function<void(const std::string& vm_id, nlohmann::json cursor)>;
    void SetCursorCallback(CursorCallback callback);
    using AudioCallback = std::function<void(const std::string& vm_id, RemoteAudioChunk chunk)>;
    void SetAudioCallback(AudioCallback callback);
    using StateChangedCallback =
        std::function<void(const std::string& vm_id, const VmRuntimeInfo& info)>;
    void SetStateChangedCallback(StateChangedCallback callback);
    // Fires once per `\n`-terminated line of runtime stdout/stderr, *after*
    // the line has been appended to the in-memory ring buffer and the
    // runtime.log file. Subscribers should treat the call as fast-fire and
    // do their own batching/throttling; the daemon will not buffer for them.
    using LogAppendCallback =
        std::function<void(const std::string& vm_id, const std::vector<std::string>& lines)>;
    void SetLogAppendCallback(LogAppendCallback callback);

    // Resolves the live LLM-proxy listen port at VM-launch time. When the
    // returned value is non-zero, BuildRuntimeArgs appends a guestfwd
    // entry mapping the canonical guest-side endpoint
    // 10.0.2.3:80 -> host 127.0.0.1:<port>, so guests can talk to the
    // host-resident OpenAI-compatible proxy at a fixed http://10.0.2.3/.
    // Set by CloudTunnel after constructing the LlmProxyService; passing
    // a null callback disables the auto-injection (e.g. for unit tests).
    using LlmProxyPortProvider = std::function<uint16_t()>;
    void SetLlmProxyPortProvider(LlmProxyPortProvider provider);

    // Clipboard event from the guest (relayed via the runtime). `type` is one
    // of "clipboard.grab" / "clipboard.data" / "clipboard.request" /
    // "clipboard.release"; selection / data_type follow the vdagent semantics
    // and `data` is the raw bytes (only set for kData).
    struct ClipboardEvent {
        std::string type;
        uint32_t selection = 0;
        uint32_t data_type = 0;
        std::vector<uint32_t> available_types;
        std::vector<uint8_t> data;
    };
    using ClipboardCallback =
        std::function<void(const std::string& vm_id, const ClipboardEvent& event)>;
    void SetClipboardCallback(const std::string& vm_id, ClipboardCallback callback);
    void ClearClipboardCallback(const std::string& vm_id);
    bool SendClipboardGrabToGuest(const std::string& vm_id,
                                  uint32_t selection,
                                  const std::vector<uint32_t>& types);
    bool SendClipboardDataToGuest(const std::string& vm_id,
                                  uint32_t selection,
                                  uint32_t data_type,
                                  const std::vector<uint8_t>& bytes);
    bool SendClipboardRequestToGuest(const std::string& vm_id,
                                     uint32_t selection,
                                     uint32_t data_type);
    bool SendClipboardReleaseToGuest(const std::string& vm_id, uint32_t selection);

    nlohmann::json Logs(const std::string& vm_id, size_t max_lines) const;

    // Returns a fresh `ProcessResources` for the VM (rss + cpu_percent),
    // sampling /proc using the shared `ProcessSampler` so consecutive callers
    // see meaningful CPU% values. Returns an empty struct if the VM is not
    // running.
    ProcessResources SampleProcessResources(const std::string& vm_id) const;

private:
    struct RuntimeSession {
        VmSpec spec;
        VmRuntimeInfo info;
        std::string control_socket;
        int process_pid = 0;
        int log_pipe_fd = -1;
        std::unique_ptr<ipc::UnixSocketServer> control_server;
        std::unique_ptr<ipc::UnixSocketConnection> runtime_conn;
        std::thread accept_thread;
        std::thread reader_thread;
        std::thread log_thread;
        std::mutex send_mutex;
        // `console_mutex` covers console history / log lines / cursor /
        // last_frame / last_audio / last_clipboard / display_state. Hot-path
        // video conversion lives under `frame_mutex` instead so that libyuv
        // ARGB→YUV conversion (a few ms per 1080p frame) does not block readers of
        // those status fields, console output, or RemoteRuntimeSnapshot.
        std::mutex console_mutex;
        std::mutex frame_mutex;
        std::vector<std::shared_ptr<ipc::UnixSocketConnection>> console_clients;
        // Long-lived RPC followers that asked for `vm.logs.follow` (the
        // `tenbox vm logs -f` CLI flag). Each line written to the in-memory
        // ring is also serialized as a `vm.logs.append` event and pushed to
        // every client in this list. Disconnected followers are reaped on
        // the next emission.
        std::vector<std::shared_ptr<ipc::UnixSocketConnection>> log_followers;
        std::string console_history;
        std::deque<std::string> log_lines;
        // Append-mode handle to `<vm_dir>/logs/runtime.log`. Lines flow into
        // both this file (for after-the-fact reads when the VM is gone) and
        // the in-memory ring buffer above (for fast drains while the VM is
        // running). Opened by StartVm after the rotation pass.
        std::ofstream log_file;
        nlohmann::json display_state = nlohmann::json::object();
        nlohmann::json last_frame = nlohmann::json::object();
        // `framebuffer`, `remote_frame`, and `remote_frame_force_full` are
        // protected by `frame_mutex`.
        std::unique_ptr<ipc::SharedFramebuffer> framebuffer;
        RemoteVideoFrame remote_frame;
        PixelFormat remote_video_format = PixelFormat::kYuv420p;
        // When true, the next reader drain will emit a full-frame slice. Set
        // on resize so the encoder's persistent input buffer can be reseeded.
        bool remote_frame_force_full = true;
        // Producers (UpdateRemoteVideoFrameLocked) notify this CV after queueing
        // new slices so consumers waiting in ReadRemoteFrame wake up immediately
        // instead of polling. Tied to `frame_mutex`.
        std::condition_variable remote_frame_cv;
        nlohmann::json cursor = nlohmann::json::object();
        nlohmann::json last_audio = nlohmann::json::object();
        nlohmann::json last_clipboard = nlohmann::json::object();
        std::atomic<bool> running{false};
        std::atomic<bool> stop_requested{false};
    };

    // Structured preflight failure. `code` is one of `kvm_unsupported`,
    // `kernel_missing`, `initrd_missing`, `disk_missing`, `permission_denied`,
    // `insufficient_memory`, `port_conflict`. `message` is human-readable
    // detail (with paths, requested vs available bytes, host:port that
    // collided, etc.).
    struct StartFailure {
        std::string code;
        std::string message;
    };
    std::optional<StartFailure> ValidateStart(const VmSpec& spec) const;
    std::vector<std::string> BuildRuntimeArgs(const VmSpec& spec, const std::string& control_socket) const;
    // Combines the persisted guest_forwards with the auto-injected LLM
    // proxy guestfwd (10.0.2.3:80 -> host 127.0.0.1:<llm_port>) when one
    // is currently bound. Used by both BuildRuntimeArgs (initial start)
    // and ApplyForwards (live runtime.update_network) so the live and
    // restart code paths agree on the effective forward set.
    std::vector<GuestForward> EffectiveGuestForwards(const VmSpec& spec) const;
    void AcceptRuntime(std::shared_ptr<RuntimeSession> session);
    void ReadRuntime(std::shared_ptr<RuntimeSession> session);
    void ReadLogs(std::shared_ptr<RuntimeSession> session);
    void HandleRuntimeMessage(std::shared_ptr<RuntimeSession> session, ipc::Message message);
    void UpdateRemoteVideoFrameLocked(RuntimeSession& session, const ipc::Message& message);
    bool SendRuntime(std::shared_ptr<RuntimeSession> session, const ipc::Message& message);
    void BroadcastConsole(std::shared_ptr<RuntimeSession> session, const std::string& data);
    void ReadConsoleClient(std::shared_ptr<RuntimeSession> session,
                           std::shared_ptr<ipc::UnixSocketConnection> client);
    std::shared_ptr<RuntimeSession> FindSession(const std::string& vm_id) const;
    void NotifyStateChanged(const std::string& vm_id, const VmRuntimeInfo& info);

    DaemonConfig config_;
    VmStore& store_;
    mutable ProcessSampler process_sampler_;
    mutable std::mutex mutex_;
    mutable std::mutex callback_mutex_;
    CursorCallback cursor_callback_;
    AudioCallback audio_callback_;
    StateChangedCallback state_changed_callback_;
    LogAppendCallback log_append_callback_;
    LlmProxyPortProvider llm_proxy_port_provider_;
    // Per-VM subscribers, keyed by vm_id. Owned by remote_webrtc / cloud_tunnel
    // for the lifetime of a remote session.
    std::map<std::string, ClipboardCallback> clipboard_callbacks_;
    std::map<std::string, std::shared_ptr<RuntimeSession>> sessions_;

    // Background worker that re-launches a runtime after the guest issues a
    // reboot (see ReadLogs). We deliberately use one long-lived thread instead
    // of spawning a short-lived `std::thread([]...).detach()` per reboot:
    // Linux's PR_SET_PDEATHSIG (which the runtime sets in its parent watcher)
    // tracks the *fork()-ing thread*, so a thread that exits right after
    // fork+exec causes the kernel to deliver SIGTERM to the freshly spawned
    // runtime, manifesting as "killed by signal 15" right after the reboot.
    void RebootWorkerLoop();
    std::thread reboot_worker_;
    std::mutex reboot_mutex_;
    std::condition_variable reboot_cv_;
    std::deque<std::string> reboot_queue_;
    bool reboot_shutdown_ = false;
};

}  // namespace tenbox::daemon
