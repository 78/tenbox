#pragma once

#include "common/ports.h"
#include "core/vdagent/vdagent_protocol.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class VirtioSerialDevice;

class VDAgentHandler {
public:
    using ClipboardCallback = std::function<void(const ClipboardEvent&)>;

    VDAgentHandler();
    ~VDAgentHandler();

    void SetSerialDevice(VirtioSerialDevice* device, uint32_t port_id);
    void SetClipboardCallback(ClipboardCallback cb) { clipboard_callback_ = std::move(cb); }

    // Process data received from guest
    void OnDataReceived(const uint8_t* data, size_t len);

    // Send clipboard grab to guest (notify of available types)
    void SendClipboardGrab(uint8_t selection, const std::vector<uint32_t>& types);

    // Send clipboard data to guest
    void SendClipboardData(uint8_t selection, uint32_t type,
                           const uint8_t* data, size_t len);

    // Request clipboard data from guest
    void SendClipboardRequest(uint8_t selection, uint32_t type);

    // Release clipboard
    void SendClipboardRelease(uint8_t selection);

    // Send announce capabilities
    void SendAnnounceCapabilities();

private:
    void ProcessMessage(const VDAgentMessage& msg, const uint8_t* data);
    void HandleAnnounceCapabilities(const uint8_t* data, uint32_t size);
    void HandleAnnounceCapabilitiesLocked(const uint8_t* data, uint32_t size);
    void HandleClipboardGrab(const uint8_t* data, uint32_t size);
    void HandleClipboardData(const uint8_t* data, uint32_t size);
    void HandleClipboardRequest(const uint8_t* data, uint32_t size);
    void HandleClipboardRelease(const uint8_t* data, uint32_t size);

    void SendMessage(uint32_t type, const uint8_t* data, size_t len);

    bool HasCapability(VDAgentCap cap) const;
    void SetCapability(uint32_t* caps, VDAgentCap cap);

    VirtioSerialDevice* serial_device_ = nullptr;
    uint32_t port_id_ = 0;
    ClipboardCallback clipboard_callback_;

    // Message reassembly buffer
    std::vector<uint8_t> recv_buffer_;
    uint32_t expected_size_ = 0;

    // Peer capabilities
    std::vector<uint32_t> guest_caps_;
    bool guest_caps_received_ = false;

    // Our capabilities
    std::vector<uint32_t> host_caps_;

    std::mutex mutex_;
};
