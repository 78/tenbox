#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class ConsolePort {
public:
    virtual ~ConsolePort() = default;

    virtual void Write(const uint8_t* data, size_t size) = 0;
    virtual size_t Read(uint8_t* out, size_t size) = 0;
};

struct PointerEvent {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t buttons = 0;
    int32_t wheel_delta = 0;  // Vertical scroll: positive=up, negative=down
};

struct KeyboardEvent {
    uint32_t key_code = 0;
    bool pressed = false;
};

class InputPort {
public:
    virtual ~InputPort() = default;
    virtual bool PollKeyboard(KeyboardEvent* event) = 0;
    virtual bool PollPointer(PointerEvent* event) = 0;
};

struct DisplayFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
    // Full resource dimensions (for dirty-rect mode)
    uint32_t resource_width = 0;
    uint32_t resource_height = 0;
    // Dirty rectangle origin within the resource
    uint32_t dirty_x = 0;
    uint32_t dirty_y = 0;
    std::vector<uint8_t> pixels;
};

struct CursorInfo {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t hot_x = 0;
    uint32_t hot_y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool visible = false;
    bool image_updated = false;
    std::vector<uint8_t> pixels;  // ARGB8888 format
};

class DisplayPort {
public:
    virtual ~DisplayPort() = default;
    virtual void SubmitFrame(const DisplayFrame& frame) = 0;
    virtual void SubmitCursor(const CursorInfo& cursor) = 0;
    virtual void SubmitScanoutState(bool active, uint32_t width, uint32_t height) = 0;
};

struct AudioChunk {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    std::vector<int16_t> pcm;
};

class AudioPort {
public:
    virtual ~AudioPort() = default;
    virtual void SubmitPcm(const AudioChunk& chunk) = 0;
};

enum class ControlCommandType : uint32_t {
    kStart = 0,
    kStop = 1,
    kReboot = 2,
    kShutdown = 3,
};

class ControlPort {
public:
    virtual ~ControlPort() = default;
    virtual void OnControlCommand(ControlCommandType command) = 0;
};

// Clipboard event for host-side handling
struct ClipboardEvent {
    enum class Type {
        kGrab,      // Clipboard content changed (with available types)
        kData,      // Clipboard data received
        kRequest,   // Request for clipboard data
        kRelease,   // Clipboard ownership released
    };

    Type type;
    uint8_t selection = 0;
    std::vector<uint32_t> available_types;  // For kGrab
    uint32_t data_type = 0;                 // For kData/kRequest
    std::vector<uint8_t> data;              // For kData
};

class ClipboardPort {
public:
    virtual ~ClipboardPort() = default;
    virtual void OnClipboardEvent(const ClipboardEvent& event) = 0;
};
