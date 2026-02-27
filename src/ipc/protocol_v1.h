#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ipc {

static constexpr uint32_t kProtocolVersion = 1;

enum class Channel : uint8_t {
    kControl = 0,
    kConsole = 1,
    kInput = 2,
    kDisplay = 3,
    kAudio = 4,
    kClipboard = 5,
};

enum class Kind : uint8_t {
    kRequest = 0,
    kResponse = 1,
    kEvent = 2,
};

struct Message {
    uint32_t version = kProtocolVersion;
    Channel channel = Channel::kControl;
    Kind kind = Kind::kRequest;
    std::string type;
    std::string vm_id;
    uint64_t request_id = 0;
    std::unordered_map<std::string, std::string> fields;
    std::vector<uint8_t> payload;
};

// Encode header line (text). When payload is non-empty the header includes
// payload_size=N and the caller must append the raw payload bytes after.
std::string EncodeHeader(const Message& message);

// Encode a complete message: header + optional raw payload bytes.
std::string Encode(const Message& message);

// Decode only the text header. Sets message.payload to empty.
// The caller is responsible for reading payload_size bytes afterwards.
std::optional<Message> Decode(const std::string& raw);

const char* ChannelToString(Channel channel);
std::optional<Channel> ChannelFromString(const std::string& value);

const char* KindToString(Kind kind);
std::optional<Kind> KindFromString(const std::string& value);

}  // namespace ipc
