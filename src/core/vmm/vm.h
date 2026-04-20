#pragma once

#include "core/vmm/types.h"
#include "core/vmm/address_space.h"
#include "core/vmm/hypervisor_vm.h"
#include "core/vmm/machine_model.h"
#include "core/vmm/vcpu_startup_state.h"
#include "core/vmm/vm_io_loop.h"
#include "core/device/virtio/virtio_mmio.h"
#include "core/device/virtio/virtio_blk.h"
#include "core/device/virtio/virtio_net.h"
#include "core/device/virtio/virtio_input.h"
#include "core/device/virtio/virtio_gpu.h"
#include "core/device/virtio/virtio_serial.h"
#include "core/device/virtio/virtio_fs.h"
#include "core/device/virtio/virtio_snd.h"
#include "core/vdagent/vdagent_handler.h"
#include "core/guest_agent/guest_agent_handler.h"
#include "core/net/net_backend.h"
#include "common/ports.h"
#include <memory>
#include <string>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

struct VmSharedFolder {
    std::string tag;
    std::string host_path;
    bool readonly = false;
};

struct VmConfig {
    std::string kernel_path;
    std::string initrd_path;
    std::string disk_path;
    std::string cmdline;
    uint64_t memory_mb = 256;
    uint32_t cpu_count = 1;
    bool net_link_up = false;
    bool debug_mode = false;
    std::vector<PortForward> port_forwards;
    std::vector<GuestForward> guest_forwards;
    std::vector<VmSharedFolder> shared_folders;
    bool interactive = true;
    std::shared_ptr<ConsolePort> console_port;
    std::shared_ptr<InputPort> input_port;
    std::shared_ptr<DisplayPort> display_port;
    std::shared_ptr<ClipboardPort> clipboard_port;
    std::shared_ptr<AudioPort> audio_port;
    uint32_t display_width = 1024;
    uint32_t display_height = 768;
};

class Vm {
public:
    ~Vm();

    static std::unique_ptr<Vm> Create(const VmConfig& config);

    int Run();

    void RequestStop();
    void RequestReboot();
    bool RebootRequested() const { return reboot_requested_.load(); }
    void TriggerPowerButton();
    void InjectConsoleBytes(const uint8_t* data, size_t size);
    void SetNetLinkUp(bool up);
    using PortForwardCallback = std::function<void(std::vector<uint16_t> failed_ports)>;
    void UpdatePortForwards(const std::vector<PortForward>& forwards,
                            PortForwardCallback cb = nullptr);
    void UpdateGuestForwards(const std::vector<GuestForward>& guest_forwards);
    void InjectKeyEvent(uint32_t evdev_code, bool pressed);
    void InjectPointerEvent(int32_t x, int32_t y, uint32_t buttons);
    void InjectWheelEvent(int32_t delta);
    void SetDisplaySize(uint32_t width, uint32_t height);

    void SendClipboardGrab(const std::vector<uint32_t>& types);
    void SendClipboardData(uint32_t type, const uint8_t* data, size_t len);
    void SendClipboardRequest(uint32_t type);
    void SendClipboardRelease();

    bool AddSharedFolder(const std::string& tag, const std::string& host_path, bool readonly = false);
    bool RemoveSharedFolder(const std::string& tag);
    std::vector<std::string> GetSharedFolderTags() const;

    GuestAgentHandler* GetGuestAgentHandler() { return guest_agent_handler_.get(); }
    bool IsGuestAgentConnected() const;
    void GuestAgentShutdown(const std::string& mode = "powerdown");
    void GuestAgentSyncTime();

private:
    Vm() = default;

    bool AllocateMemory(uint64_t size);
    bool SetupVirtioBlk(const std::string& disk_path, const VirtioDeviceSlot& slot);
    bool SetupVirtioNet(bool link_up, const std::vector<PortForward>& forwards,
                        const std::vector<GuestForward>& guest_forwards, const VirtioDeviceSlot& slot);
    bool SetupVirtioInput(const VirtioDeviceSlot& kbd_slot, const VirtioDeviceSlot& tablet_slot);
    bool SetupVirtioGpu(uint32_t width, uint32_t height, const VirtioDeviceSlot& slot);
    bool SetupVirtioSerial(const VirtioDeviceSlot& slot);
    bool SetupVirtioFs(const std::vector<VmSharedFolder>& initial_folders, const VirtioDeviceSlot& slot);
    bool SetupVirtioSnd(const VirtioDeviceSlot& slot);

    void VCpuThreadFunc(uint32_t vcpu_index);
    void SetupVCpuCallbacks(uint32_t vcpu_index);
    void FinalizeBoot(const VmConfig& config);
    void InjectIrq(uint8_t irq);
    void SetIrqLevel(uint8_t irq, bool asserted);

    // Record a virtio-mmio slot as an IRQFD candidate. The actual KVM_IRQFD
    // registration happens inside Run() once vCPUs (and, on arm64, the VGIC)
    // are up. The classic SetIrqLevelCallback path stays wired as a fallback;
    // when the real fd is installed the device transparently switches over.
    bool TryEnableIrqFd(VirtioMmioDevice* dev, uint8_t slot_irq);

    // Register all recorded candidate slots with the hypervisor + io_loop_.
    // Slots that fail stay in the callback-driven path. Linux-only.
    void InstallIrqFds();
    void ShutdownIrqFds();

    uint32_t cpu_count_ = 1;
    std::unique_ptr<MachineModel> machine_;
    std::unique_ptr<HypervisorVm> hv_vm_;
    std::vector<std::unique_ptr<HypervisorVCpu>> vcpus_;
    std::vector<std::thread> vcpu_threads_;
    std::atomic<int> exit_code_{0};

    GuestMemMap mem_;
    AddressSpace addr_space_;

    std::unique_ptr<VirtioBlkDevice> virtio_blk_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_;

    std::unique_ptr<VirtioNetDevice> virtio_net_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_net_;
    std::unique_ptr<NetBackend> net_backend_;

    std::unique_ptr<VirtioInputDevice> virtio_kbd_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_kbd_;
    std::unique_ptr<VirtioInputDevice> virtio_tablet_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_tablet_;

    std::unique_ptr<VirtioGpuDevice> virtio_gpu_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_gpu_;

    std::unique_ptr<VirtioSerialDevice> virtio_serial_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_serial_;
    std::unique_ptr<VDAgentHandler> vdagent_handler_;
    std::unique_ptr<GuestAgentHandler> guest_agent_handler_;

    std::unique_ptr<VirtioFsDevice> virtio_fs_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_fs_;

    std::unique_ptr<VirtioSndDevice> virtio_snd_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_snd_;

    // Active virtio slot list (populated during setup, used for kernel loading)
    std::vector<VirtioDeviceSlot> active_virtio_slots_;

    struct IrqFdSlot {
        uint32_t gsi = 0;             // absolute hypervisor GSI
        int trigger_fd = -1;          // write-to-assert eventfd
        int resample_fd = -1;         // signalled on EOI (may be -1)
        VirtioMmioDevice* dev = nullptr;  // for pending-status re-check
    };
    std::vector<IrqFdSlot> irqfd_slots_;
    VmIoLoop io_loop_;

    std::atomic<bool> running_{false};
    std::atomic<bool> reboot_requested_{false};

    // Two-phase boot barrier: threads signal ready after vCPU creation;
    // main thread does FinalizeBoot then releases them.
    std::atomic<uint32_t> vcpus_ready_{0};
    std::mutex boot_mutex_;
    std::condition_variable boot_cv_;
    bool boot_complete_ = false;

    // Saved config for FinalizeBoot (cmdline, kernel path, etc.)
    VmConfig boot_config_;
    std::shared_ptr<ConsolePort> console_port_;
    std::shared_ptr<InputPort> input_port_;
    std::shared_ptr<DisplayPort> display_port_;
    std::shared_ptr<ClipboardPort> clipboard_port_;
    std::shared_ptr<AudioPort> audio_port_;
    uint32_t inject_prev_buttons_ = 0;

    std::vector<std::unique_ptr<VCpuStartupState>> vcpu_startup_;

    // Input pump thread for locally-created ConsolePort (CLI interactive mode).
    // Inactive when running under an external IPC controller that injects
    // console bytes itself.
    std::thread console_input_thread_;
    bool owned_console_input_ = false;
};
