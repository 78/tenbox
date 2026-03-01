#pragma once

#include <cstdint>

// VD Agent protocol definitions based on SPICE protocol specification

// VD Agent message types
enum VDAgentMessageType : uint32_t {
    VD_AGENT_MOUSE_STATE           = 1,
    VD_AGENT_MONITORS_CONFIG       = 2,
    VD_AGENT_REPLY                 = 3,
    VD_AGENT_CLIPBOARD             = 4,
    VD_AGENT_DISPLAY_CONFIG        = 5,
    VD_AGENT_ANNOUNCE_CAPABILITIES = 6,
    VD_AGENT_CLIPBOARD_GRAB        = 7,
    VD_AGENT_CLIPBOARD_REQUEST     = 8,
    VD_AGENT_CLIPBOARD_RELEASE     = 9,
    VD_AGENT_FILE_XFER_START       = 10,
    VD_AGENT_FILE_XFER_STATUS      = 11,
    VD_AGENT_FILE_XFER_DATA        = 12,
    VD_AGENT_CLIENT_DISCONNECTED   = 13,
    VD_AGENT_MAX_CLIPBOARD         = 14,
    VD_AGENT_AUDIO_VOLUME_SYNC     = 15,
    VD_AGENT_GRAPHICS_DEVICE_INFO  = 16,
};

// VD Agent clipboard types
enum VDAgentClipboardType : uint32_t {
    VD_AGENT_CLIPBOARD_NONE        = 0,
    VD_AGENT_CLIPBOARD_UTF8_TEXT   = 1,
    VD_AGENT_CLIPBOARD_IMAGE_PNG   = 2,
    VD_AGENT_CLIPBOARD_IMAGE_BMP   = 3,
    VD_AGENT_CLIPBOARD_IMAGE_TIFF  = 4,
    VD_AGENT_CLIPBOARD_IMAGE_JPG   = 5,
    VD_AGENT_CLIPBOARD_FILE_LIST   = 6,
};

// VD Agent clipboard selection (for X11 compatibility)
enum VDAgentClipboardSelection : uint8_t {
    VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD = 0,
    VD_AGENT_CLIPBOARD_SELECTION_PRIMARY   = 1,
    VD_AGENT_CLIPBOARD_SELECTION_SECONDARY = 2,
};

// Agent capabilities
enum VDAgentCap : uint32_t {
    VD_AGENT_CAP_MOUSE_STATE           = 0,
    VD_AGENT_CAP_MONITORS_CONFIG       = 1,
    VD_AGENT_CAP_REPLY                 = 2,
    VD_AGENT_CAP_CLIPBOARD             = 3,
    VD_AGENT_CAP_DISPLAY_CONFIG        = 4,
    VD_AGENT_CAP_CLIPBOARD_BY_DEMAND   = 5,
    VD_AGENT_CAP_CLIPBOARD_SELECTION   = 6,
    VD_AGENT_CAP_SPARSE_MONITORS_CONFIG = 7,
    VD_AGENT_CAP_GUEST_LINEEND_LF      = 8,
    VD_AGENT_CAP_GUEST_LINEEND_CRLF    = 9,
    VD_AGENT_CAP_MAX_CLIPBOARD         = 10,
    VD_AGENT_CAP_AUDIO_VOLUME_SYNC     = 11,
    VD_AGENT_CAP_MONITORS_CONFIG_POSITION = 12,
    VD_AGENT_CAP_FILE_XFER_DISABLED    = 13,
    VD_AGENT_CAP_FILE_XFER_DETAILED_ERRORS = 14,
    VD_AGENT_CAP_GRAPHICS_DEVICE_INFO  = 15,
    VD_AGENT_CAP_CLIPBOARD_NO_RELEASE_ON_REGRAB = 16,
    VD_AGENT_CAP_CLIPBOARD_GRAB_SERIAL = 17,
};

#pragma pack(push, 1)

// Chunk header for VD Agent protocol over virtio-serial
struct VDAgentChunkHeader {
    uint32_t port;      // Always 1 for vdagent
    uint32_t size;      // Size of the message (including VDAgentMessage header)
};

// VD Agent message header
struct VDAgentMessage {
    uint32_t protocol;  // VD_AGENT_PROTOCOL (1)
    uint32_t type;      // VDAgentMessageType
    uint64_t opaque;    // Opaque data for client
    uint32_t size;      // Size of message data following this header
};

// Announce capabilities message
struct VDAgentAnnounceCapabilities {
    uint32_t request;   // 1 if requesting caps from peer, 0 if just announcing
    uint32_t caps[1];   // Variable-length capability bits
};

// Clipboard grab message (with selection support)
struct VDAgentClipboardGrab {
    uint8_t selection;  // VDAgentClipboardSelection
    uint8_t reserved[3];
    uint32_t types[1];  // Variable-length list of clipboard types
};

// Clipboard grab message (without selection support, legacy)
struct VDAgentClipboardGrabLegacy {
    uint32_t types[1];  // Variable-length list of clipboard types
};

// Clipboard request message
struct VDAgentClipboardRequest {
    uint8_t selection;  // VDAgentClipboardSelection
    uint8_t reserved[3];
    uint32_t type;      // Requested clipboard type
};

// Clipboard request message (legacy)
struct VDAgentClipboardRequestLegacy {
    uint32_t type;      // Requested clipboard type
};

// Clipboard data message
struct VDAgentClipboard {
    uint8_t selection;  // VDAgentClipboardSelection
    uint8_t reserved[3];
    uint32_t type;      // Clipboard type
    // Followed by clipboard data
};

// Clipboard data message (legacy)
struct VDAgentClipboardLegacy {
    uint32_t type;      // Clipboard type
    // Followed by clipboard data
};

// Clipboard release message
struct VDAgentClipboardRelease {
    uint8_t selection;  // VDAgentClipboardSelection
    uint8_t reserved[3];
};

#pragma pack(pop)

constexpr uint32_t VD_AGENT_PROTOCOL = 1;
constexpr uint32_t VD_AGENT_MAX_DATA_SIZE = 1024 * 1024;  // 1MB max clipboard size
constexpr uint32_t VD_AGENT_MAX_CHUNK_SIZE = 2048;         // Max payload per chunk over virtio-serial
