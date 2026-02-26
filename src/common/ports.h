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

class DisplayPort {
public:
    virtual ~DisplayPort() = default;
    virtual void SubmitFrame(const DisplayFrame& frame) = 0;
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
