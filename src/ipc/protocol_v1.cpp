#include "ipc/protocol_v1.h"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace ipc {
namespace {

std::string Escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '\\' || ch == '\t' || ch == '\n' || ch == '=') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

std::string Unescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    bool escaped = false;
    for (char ch : value) {
        if (escaped) {
            out.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

std::vector<std::string> SplitTabs(const std::string& value) {
    std::vector<std::string> out;
    std::string token;
    bool escaped = false;
    for (char ch : value) {
        if (escaped) {
            token.push_back('\\');
            token.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '\t') {
            out.push_back(token);
            token.clear();
            continue;
        }
        token.push_back(ch);
    }
    if (escaped) token.push_back('\\');
    out.push_back(token);
    return out;
}

std::optional<uint64_t> ParseU64(const std::string& value) {
    if (value.empty()) return std::nullopt;
    char* end = nullptr;
    const auto number = std::strtoull(value.c_str(), &end, 10);
    if (!end || *end != '\0') {
        return std::nullopt;
    }
    return number;
}

}  // namespace

const char* ChannelToString(Channel channel) {
    switch (channel) {
    case Channel::kControl: return "control";
    case Channel::kConsole: return "console";
    case Channel::kInput: return "input";
    case Channel::kDisplay: return "display";
    case Channel::kAudio: return "audio";
    case Channel::kClipboard: return "clipboard";
    default: return "unknown";
    }
}

std::optional<Channel> ChannelFromString(const std::string& value) {
    if (value == "control") return Channel::kControl;
    if (value == "console") return Channel::kConsole;
    if (value == "input") return Channel::kInput;
    if (value == "display") return Channel::kDisplay;
    if (value == "audio") return Channel::kAudio;
    if (value == "clipboard") return Channel::kClipboard;
    return std::nullopt;
}

const char* KindToString(Kind kind) {
    switch (kind) {
    case Kind::kRequest: return "request";
    case Kind::kResponse: return "response";
    case Kind::kEvent: return "event";
    default: return "unknown";
    }
}

std::optional<Kind> KindFromString(const std::string& value) {
    if (value == "request") return Kind::kRequest;
    if (value == "response") return Kind::kResponse;
    if (value == "event") return Kind::kEvent;
    return std::nullopt;
}

std::string EncodeHeader(const Message& message) {
    std::ostringstream oss;
    oss << "version=" << message.version
        << '\t' << "channel=" << Escape(ChannelToString(message.channel))
        << '\t' << "kind=" << Escape(KindToString(message.kind))
        << '\t' << "type=" << Escape(message.type)
        << '\t' << "vm_id=" << Escape(message.vm_id)
        << '\t' << "request_id=" << message.request_id;

    for (const auto& [key, value] : message.fields) {
        oss << '\t' << Escape(key) << '=' << Escape(value);
    }

    if (!message.payload.empty()) {
        oss << '\t' << "payload_size=" << message.payload.size();
    }

    oss << '\n';
    return oss.str();
}

std::string Encode(const Message& message) {
    std::string out = EncodeHeader(message);
    if (!message.payload.empty()) {
        out.append(reinterpret_cast<const char*>(message.payload.data()),
                   message.payload.size());
    }
    return out;
}

std::optional<Message> Decode(const std::string& raw) {
    Message message;
    std::string line = raw;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }

    const auto tokens = SplitTabs(line);
    if (tokens.size() < 6) {
        return std::nullopt;
    }

    for (const auto& token : tokens) {
        const auto sep = token.find('=');
        if (sep == std::string::npos) {
            continue;
        }

        const std::string key = Unescape(token.substr(0, sep));
        const std::string value = Unescape(token.substr(sep + 1));

        if (key == "version") {
            const auto parsed = ParseU64(value);
            if (!parsed) return std::nullopt;
            message.version = static_cast<uint32_t>(*parsed);
        } else if (key == "channel") {
            const auto parsed = ChannelFromString(value);
            if (!parsed) return std::nullopt;
            message.channel = *parsed;
        } else if (key == "kind") {
            const auto parsed = KindFromString(value);
            if (!parsed) return std::nullopt;
            message.kind = *parsed;
        } else if (key == "type") {
            message.type = value;
        } else if (key == "vm_id") {
            message.vm_id = value;
        } else if (key == "request_id") {
            const auto parsed = ParseU64(value);
            if (!parsed) return std::nullopt;
            message.request_id = *parsed;
        } else {
            message.fields.emplace(key, value);
        }
    }

    if (message.version != kProtocolVersion) {
        return std::nullopt;
    }
    if (message.type.empty()) {
        return std::nullopt;
    }
    return message;
}

}  // namespace ipc
