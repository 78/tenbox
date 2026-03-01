#include "core/vdagent/vdagent_handler.h"
#include "core/device/virtio/virtio_serial.h"
#include "core/vmm/types.h"
#include <cstring>
#include <algorithm>

VDAgentHandler::VDAgentHandler() {
    // Initialize our capabilities
    host_caps_.resize(1);
    SetCapability(host_caps_.data(), VD_AGENT_CAP_CLIPBOARD);
    SetCapability(host_caps_.data(), VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
    SetCapability(host_caps_.data(), VD_AGENT_CAP_CLIPBOARD_SELECTION);
    SetCapability(host_caps_.data(), VD_AGENT_CAP_GUEST_LINEEND_CRLF);
}

VDAgentHandler::~VDAgentHandler() = default;

void VDAgentHandler::SetSerialDevice(VirtioSerialDevice* device, uint32_t port_id) {
    serial_device_ = device;
    port_id_ = port_id;
}

bool VDAgentHandler::HasCapability(VDAgentCap cap) const {
    uint32_t word = cap / 32;
    uint32_t bit = cap % 32;
    if (word >= guest_caps_.size()) return false;
    return (guest_caps_[word] & (1u << bit)) != 0;
}

void VDAgentHandler::SetCapability(uint32_t* caps, VDAgentCap cap) {
    uint32_t word = cap / 32;
    uint32_t bit = cap % 32;
    caps[word] |= (1u << bit);
}

void VDAgentHandler::OnDataReceived(const uint8_t* data, size_t len) {
    bool need_send_caps = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        recv_buffer_.insert(recv_buffer_.end(), data, data + len);

        while (true) {
            if (recv_buffer_.size() < sizeof(VDAgentChunkHeader)) {
                break;
            }

            VDAgentChunkHeader chunk;
            std::memcpy(&chunk, recv_buffer_.data(), sizeof(chunk));

            size_t total_size = sizeof(VDAgentChunkHeader) + chunk.size;
            if (recv_buffer_.size() < total_size) {
                break;
            }

            const uint8_t* chunk_payload = recv_buffer_.data() + sizeof(VDAgentChunkHeader);

            if (!has_pending_msg_) {
                // First chunk of a new message - must contain VDAgentMessage header
                if (chunk.size < sizeof(VDAgentMessage)) {
                    recv_buffer_.erase(recv_buffer_.begin(), recv_buffer_.begin() + total_size);
                    continue;
                }

                std::memcpy(&pending_msg_, chunk_payload, sizeof(VDAgentMessage));
                pending_data_.clear();

                uint32_t data_in_chunk = chunk.size - static_cast<uint32_t>(sizeof(VDAgentMessage));
                const uint8_t* payload_start = chunk_payload + sizeof(VDAgentMessage);
                pending_data_.insert(pending_data_.end(), payload_start, payload_start + data_in_chunk);
                has_pending_msg_ = true;
            } else {
                // Continuation chunk - raw payload only
                pending_data_.insert(pending_data_.end(), chunk_payload, chunk_payload + chunk.size);
            }

            recv_buffer_.erase(recv_buffer_.begin(), recv_buffer_.begin() + total_size);

            // Check if we have collected the full message
            if (pending_data_.size() >= pending_msg_.size) {
                has_pending_msg_ = false;

                if (pending_msg_.type == VD_AGENT_ANNOUNCE_CAPABILITIES) {
                    uint32_t request = 0;
                    if (pending_msg_.size >= sizeof(uint32_t)) {
                        std::memcpy(&request, pending_data_.data(), sizeof(request));
                    }
                    HandleAnnounceCapabilitiesLocked(pending_data_.data(), pending_msg_.size);
                    if (request) {
                        need_send_caps = true;
                    }
                } else {
                    ProcessMessage(pending_msg_, pending_data_.data());
                }

                pending_data_.clear();
            }
        }
    }

    if (need_send_caps) {
        SendAnnounceCapabilities();
    }
}

void VDAgentHandler::ProcessMessage(const VDAgentMessage& msg, const uint8_t* data) {
    LOG_DEBUG("VDAgent: received message type=%u size=%u", msg.type, msg.size);

    switch (msg.type) {
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        HandleAnnounceCapabilities(data, msg.size);
        break;
    case VD_AGENT_CLIPBOARD_GRAB:
        HandleClipboardGrab(data, msg.size);
        break;
    case VD_AGENT_CLIPBOARD:
        HandleClipboardData(data, msg.size);
        break;
    case VD_AGENT_CLIPBOARD_REQUEST:
        HandleClipboardRequest(data, msg.size);
        break;
    case VD_AGENT_CLIPBOARD_RELEASE:
        HandleClipboardRelease(data, msg.size);
        break;
    default:
        LOG_DEBUG("VDAgent: unhandled message type %u", msg.type);
        break;
    }
}

void VDAgentHandler::HandleAnnounceCapabilitiesLocked(const uint8_t* data, uint32_t size) {
    if (size < sizeof(uint32_t)) return;

    uint32_t caps_size = size - sizeof(uint32_t);
    uint32_t num_caps = caps_size / sizeof(uint32_t);

    guest_caps_.resize(num_caps);
    if (num_caps > 0) {
        std::memcpy(guest_caps_.data(), data + sizeof(uint32_t), num_caps * sizeof(uint32_t));
    }
    guest_caps_received_ = true;

    LOG_INFO("VDAgent: guest capabilities received (%u words)", num_caps);
}

void VDAgentHandler::HandleAnnounceCapabilities(const uint8_t* data, uint32_t size) {
    if (size < sizeof(uint32_t)) return;

    uint32_t request;
    std::memcpy(&request, data, sizeof(request));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        HandleAnnounceCapabilitiesLocked(data, size);
    }

    // If guest requested our capabilities, send them
    if (request) {
        SendAnnounceCapabilities();
    }
}

void VDAgentHandler::HandleClipboardGrab(const uint8_t* data, uint32_t size) {
    ClipboardEvent event;
    event.type = ClipboardEvent::Type::kGrab;

    if (HasCapability(VD_AGENT_CAP_CLIPBOARD_SELECTION) && size >= 4) {
        event.selection = data[0];
        const uint32_t* types = reinterpret_cast<const uint32_t*>(data + 4);
        uint32_t num_types = (size - 4) / sizeof(uint32_t);
        event.available_types.assign(types, types + num_types);
    } else {
        event.selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
        const uint32_t* types = reinterpret_cast<const uint32_t*>(data);
        uint32_t num_types = size / sizeof(uint32_t);
        event.available_types.assign(types, types + num_types);
    }

    LOG_INFO("VDAgent: clipboard grab, selection=%u, %zu types",
             event.selection, event.available_types.size());

    if (clipboard_callback_) {
        clipboard_callback_(event);
    }
}

void VDAgentHandler::HandleClipboardData(const uint8_t* data, uint32_t size) {
    ClipboardEvent event;
    event.type = ClipboardEvent::Type::kData;

    if (HasCapability(VD_AGENT_CAP_CLIPBOARD_SELECTION) && size >= 8) {
        event.selection = data[0];
        std::memcpy(&event.data_type, data + 4, sizeof(uint32_t));
        event.data.assign(data + 8, data + size);
    } else if (size >= 4) {
        event.selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
        std::memcpy(&event.data_type, data, sizeof(uint32_t));
        event.data.assign(data + 4, data + size);
    } else {
        return;
    }

    LOG_INFO("VDAgent: clipboard data, selection=%u, type=%u, size=%zu",
             event.selection, event.data_type, event.data.size());

    if (clipboard_callback_) {
        clipboard_callback_(event);
    }
}

void VDAgentHandler::HandleClipboardRequest(const uint8_t* data, uint32_t size) {
    ClipboardEvent event;
    event.type = ClipboardEvent::Type::kRequest;

    if (HasCapability(VD_AGENT_CAP_CLIPBOARD_SELECTION) && size >= 8) {
        event.selection = data[0];
        std::memcpy(&event.data_type, data + 4, sizeof(uint32_t));
    } else if (size >= 4) {
        event.selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
        std::memcpy(&event.data_type, data, sizeof(uint32_t));
    } else {
        return;
    }

    LOG_INFO("VDAgent: clipboard request, selection=%u, type=%u",
             event.selection, event.data_type);

    if (clipboard_callback_) {
        clipboard_callback_(event);
    }
}

void VDAgentHandler::HandleClipboardRelease(const uint8_t* data, uint32_t size) {
    ClipboardEvent event;
    event.type = ClipboardEvent::Type::kRelease;

    if (HasCapability(VD_AGENT_CAP_CLIPBOARD_SELECTION) && size >= 1) {
        event.selection = data[0];
    } else {
        event.selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    }

    LOG_INFO("VDAgent: clipboard release, selection=%u", event.selection);

    if (clipboard_callback_) {
        clipboard_callback_(event);
    }
}

void VDAgentHandler::SendMessage(uint32_t type, const uint8_t* data, size_t len) {
    if (!serial_device_) return;

    VDAgentMessage msg;
    msg.protocol = VD_AGENT_PROTOCOL;
    msg.type = type;
    msg.opaque = 0;
    msg.size = static_cast<uint32_t>(len);

    // First chunk carries the VDAgentMessage header + as much payload as fits
    const uint32_t max_payload = VD_AGENT_MAX_CHUNK_SIZE;
    uint32_t first_data_len = std::min(static_cast<uint32_t>(len),
                                       max_payload - static_cast<uint32_t>(sizeof(VDAgentMessage)));

    {
        VDAgentChunkHeader chunk;
        chunk.port = 1;
        chunk.size = static_cast<uint32_t>(sizeof(VDAgentMessage)) + first_data_len;

        std::vector<uint8_t> buffer(sizeof(VDAgentChunkHeader) + chunk.size);
        std::memcpy(buffer.data(), &chunk, sizeof(chunk));
        std::memcpy(buffer.data() + sizeof(chunk), &msg, sizeof(msg));
        if (first_data_len > 0 && data) {
            std::memcpy(buffer.data() + sizeof(chunk) + sizeof(msg), data, first_data_len);
        }

        serial_device_->SendData(port_id_, buffer.data(), buffer.size());
    }

    // Subsequent chunks carry only raw payload data (no VDAgentMessage header)
    size_t offset = first_data_len;
    while (offset < len) {
        uint32_t chunk_data_len = std::min(static_cast<uint32_t>(len - offset), max_payload);

        VDAgentChunkHeader chunk;
        chunk.port = 1;
        chunk.size = chunk_data_len;

        std::vector<uint8_t> buffer(sizeof(VDAgentChunkHeader) + chunk_data_len);
        std::memcpy(buffer.data(), &chunk, sizeof(chunk));
        std::memcpy(buffer.data() + sizeof(chunk), data + offset, chunk_data_len);

        serial_device_->SendData(port_id_, buffer.data(), buffer.size());
        offset += chunk_data_len;
    }
}

void VDAgentHandler::SendAnnounceCapabilities() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<uint8_t> data;
    data.resize(sizeof(uint32_t) + host_caps_.size() * sizeof(uint32_t));

    uint32_t request = 0;
    std::memcpy(data.data(), &request, sizeof(request));
    std::memcpy(data.data() + sizeof(uint32_t), host_caps_.data(),
                host_caps_.size() * sizeof(uint32_t));

    SendMessage(VD_AGENT_ANNOUNCE_CAPABILITIES, data.data(), data.size());
    LOG_INFO("VDAgent: sent announce capabilities");
}

void VDAgentHandler::SendClipboardGrab(uint8_t selection,
                                        const std::vector<uint32_t>& types) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!guest_caps_received_) {
        LOG_DEBUG("VDAgent: guest caps not received, skipping clipboard grab");
        return;
    }

    std::vector<uint8_t> data;

    if (HasCapability(VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        data.resize(4 + types.size() * sizeof(uint32_t));
        data[0] = selection;
        data[1] = data[2] = data[3] = 0;
        std::memcpy(data.data() + 4, types.data(), types.size() * sizeof(uint32_t));
    } else {
        data.resize(types.size() * sizeof(uint32_t));
        std::memcpy(data.data(), types.data(), types.size() * sizeof(uint32_t));
    }

    SendMessage(VD_AGENT_CLIPBOARD_GRAB, data.data(), data.size());
    LOG_INFO("VDAgent: sent clipboard grab with %zu types", types.size());
}

void VDAgentHandler::SendClipboardData(uint8_t selection, uint32_t type,
                                        const uint8_t* data_ptr, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!guest_caps_received_) return;

    std::vector<uint8_t> data;

    if (HasCapability(VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        data.resize(8 + len);
        data[0] = selection;
        data[1] = data[2] = data[3] = 0;
        std::memcpy(data.data() + 4, &type, sizeof(type));
        if (len > 0 && data_ptr) {
            std::memcpy(data.data() + 8, data_ptr, len);
        }
    } else {
        data.resize(4 + len);
        std::memcpy(data.data(), &type, sizeof(type));
        if (len > 0 && data_ptr) {
            std::memcpy(data.data() + 4, data_ptr, len);
        }
    }

    SendMessage(VD_AGENT_CLIPBOARD, data.data(), data.size());
    LOG_INFO("VDAgent: sent clipboard data type=%u size=%zu", type, len);
}

void VDAgentHandler::SendClipboardRequest(uint8_t selection, uint32_t type) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!guest_caps_received_) return;

    std::vector<uint8_t> data;

    if (HasCapability(VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        data.resize(8);
        data[0] = selection;
        data[1] = data[2] = data[3] = 0;
        std::memcpy(data.data() + 4, &type, sizeof(type));
    } else {
        data.resize(4);
        std::memcpy(data.data(), &type, sizeof(type));
    }

    SendMessage(VD_AGENT_CLIPBOARD_REQUEST, data.data(), data.size());
    LOG_INFO("VDAgent: sent clipboard request type=%u", type);
}

void VDAgentHandler::SendClipboardRelease(uint8_t selection) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!guest_caps_received_) return;

    std::vector<uint8_t> data;

    if (HasCapability(VD_AGENT_CAP_CLIPBOARD_SELECTION)) {
        data.resize(4);
        data[0] = selection;
        data[1] = data[2] = data[3] = 0;
    }

    SendMessage(VD_AGENT_CLIPBOARD_RELEASE, data.data(), data.size());
    LOG_INFO("VDAgent: sent clipboard release");
}
