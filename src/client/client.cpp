#include "client/client.h"

#include "ipc/unix_socket.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace tenbox::client {
namespace {

class RawTerminal {
public:
    RawTerminal() {
        if (::tcgetattr(STDIN_FILENO, &saved_) == 0) {
            termios raw = saved_;
            raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO | ISIG));
            raw.c_iflag &= static_cast<unsigned>(~(IXON | ICRNL));
            raw.c_oflag &= static_cast<unsigned>(~OPOST);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            enabled_ = (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0);
        }
    }

    ~RawTerminal() {
        Restore();
    }

    void Restore() {
        if (enabled_) {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
            enabled_ = false;
        }
    }

private:
    termios saved_{};
    bool enabled_ = false;
};

std::string ReadJsonLine(ipc::UnixSocketConnection& conn) {
    return conn.ReadLine();
}

bool WriteJsonLine(ipc::UnixSocketConnection& conn, const nlohmann::json& value) {
    std::string line = value.dump();
    line.push_back('\n');
    return conn.Send(line);
}

std::string BytesToHex(const char* data, size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(size * 2, '\0');
    for (size_t i = 0; i < size; ++i) {
        const auto byte = static_cast<unsigned char>(data[i]);
        out[i * 2] = kDigits[byte >> 4];
        out[i * 2 + 1] = kDigits[byte & 0x0f];
    }
    return out;
}

std::string HexToBytes(const std::string& hex) {
    auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
    };

    if ((hex.size() % 2) != 0) return {};
    std::string out(hex.size() / 2, '\0');
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = digit(hex[i * 2]);
        const int lo = digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return {};
        out[i] = static_cast<char>((hi << 4) | lo);
    }
    return out;
}

std::string StripHostTerminalQueries(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size();) {
        const unsigned char ch = static_cast<unsigned char>(bytes[i]);
        if (ch == 0x1b && i + 1 < bytes.size()) {
            if (bytes[i + 1] == '[') {
                size_t j = i + 2;
                while (j < bytes.size()) {
                    const unsigned char c = static_cast<unsigned char>(bytes[j]);
                    if (c >= 0x40 && c <= 0x7e) break;
                    ++j;
                }
                if (j < bytes.size() && (bytes[j] == 'n' || bytes[j] == 'c')) {
                    i = j + 1;
                    continue;
                }
                const std::string seq = bytes.substr(i, j < bytes.size() ? (j - i + 1) : 0);
                if (seq == "\x1b[!p" || seq == "\x1b[?7h" ||
                    seq == "\x1b[32766;32766H") {
                    i = j + 1;
                    continue;
                }
            } else if (bytes[i + 1] == ']') {
                // Drop OSC sequences so guest color/title queries do not get
                // answered by the host terminal and echoed back into the VM.
                size_t j = i + 2;
                while (j < bytes.size()) {
                    if (bytes[j] == '\a') {
                        ++j;
                        break;
                    }
                    if (bytes[j] == 0x1b && j + 1 < bytes.size() && bytes[j + 1] == '\\') {
                        j += 2;
                        break;
                    }
                    ++j;
                }
                if (j <= bytes.size()) {
                    i = j;
                    continue;
                }
            } else if (bytes[i + 1] == 'P') {
                // Drop DCS queries/responses for the same reason.
                size_t j = i + 2;
                while (j + 1 < bytes.size()) {
                    if (bytes[j] == 0x1b && bytes[j + 1] == '\\') {
                        j += 2;
                        break;
                    }
                    ++j;
                }
                if (j <= bytes.size()) {
                    i = j;
                    continue;
                }
            }
        }
        out.push_back(bytes[i++]);
    }
    return out;
}

std::string StripHostTerminalResponses(const char* data, size_t size) {
    std::string out;
    out.reserve(size);
    for (size_t i = 0; i < size;) {
        const unsigned char ch = static_cast<unsigned char>(data[i]);
        if (ch == 0x1b && i + 1 < size) {
            if (data[i + 1] == '[') {
                size_t j = i + 2;
                while (j < size) {
                    const unsigned char c = static_cast<unsigned char>(data[j]);
                    if (c >= 0x40 && c <= 0x7e) break;
                    ++j;
                }
                if (j < size && (data[j] == 'R' || data[j] == 'c')) {
                    i = j + 1;
                    continue;
                }
            } else if (data[i + 1] == ']') {
                size_t j = i + 2;
                while (j < size) {
                    if (data[j] == '\a') {
                        ++j;
                        break;
                    }
                    if (data[j] == 0x1b && j + 1 < size && data[j + 1] == '\\') {
                        j += 2;
                        break;
                    }
                    ++j;
                }
                if (j <= size) {
                    i = j;
                    continue;
                }
            } else if (data[i + 1] == 'P') {
                size_t j = i + 2;
                while (j + 1 < size) {
                    if (data[j] == 0x1b && data[j + 1] == '\\') {
                        j += 2;
                        break;
                    }
                    ++j;
                }
                if (j <= size) {
                    i = j;
                    continue;
                }
            }
        }
        out.push_back(data[i++]);
    }
    return out;
}

void HandleConsoleEvent(const nlohmann::json& event) {
    if (event.value("type", "") == "console.data") {
        const std::string bytes = StripHostTerminalQueries(HexToBytes(event.value("data_hex", "")));
        if (!bytes.empty()) {
            (void)::write(STDOUT_FILENO, bytes.data(), bytes.size());
        }
    }
}

void PrintDetached(RawTerminal& raw) {
    raw.Restore();
    std::cerr << "\r\nDetached.\r\n";
}

}  // namespace

std::string DefaultSocketPath() {
    // 1. Explicit override always wins.
    if (const char* explicit_path = std::getenv("TENBOX_SOCK")) {
        if (*explicit_path) return explicit_path;
    }

    // 2. System-wide tenboxd installed via the .deb listens at
    //    /run/tenbox/tenbox.sock and chmod's it 0660 + chgrp'd to
    //    `tenbox`. We prefer that path whenever the system daemon is
    //    plausibly present, so users in the tenbox group don't have
    //    to set TENBOX_SOCK by hand.
    //
    //    The decision is "does /run/tenbox/ exist?" — not "can I read
    //    the socket?" — for two reasons:
    //      * /run/tenbox/ is 0755 (world-traversable), so the stat
    //        on the socket itself succeeds for everyone.
    //      * If the user isn't in the `tenbox` group the connect()
    //        will fail with EACCES, which is a far more useful error
    //        than silently falling back to $XDG_RUNTIME_DIR/tenbox.sock
    //        and surfacing "no such file or directory" — that
    //        misdirection cost an hour of head-scratching once
    //        already.
    {
        struct stat dir_st;
        if (::stat("/run/tenbox", &dir_st) == 0 && S_ISDIR(dir_st.st_mode)) {
            return "/run/tenbox/tenbox.sock";
        }
    }

    // 3. Per-user dev daemon (just `./tenboxd` in a terminal) lands
    //    here because tenboxd's own DefaultSocketPath() honors
    //    XDG_RUNTIME_DIR.
    if (const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        if (*runtime_dir) return std::string(runtime_dir) + "/tenbox.sock";
    }

    // 4. Last resort for environments without XDG_RUNTIME_DIR (cron,
    //    minimal containers).
    return "/tmp/tenbox-" + std::to_string(::getuid()) + ".sock";
}

Client::Client(std::string socket_path) : socket_path_(std::move(socket_path)) {}

Response Client::Request(const nlohmann::json& request) const {
    auto conn = ipc::UnixSocketClient::Connect(socket_path_);
    if (!conn.IsValid()) {
        return {.ok = false, .error = "failed to connect to " + socket_path_};
    }
    if (!WriteJsonLine(conn, request)) {
        return {.ok = false, .error = "failed to send request"};
    }

    const std::string line = ReadJsonLine(conn);
    if (line.empty()) {
        return {.ok = false, .error = "daemon closed the connection"};
    }

    auto body = nlohmann::json::parse(line, nullptr, false);
    if (body.is_discarded()) {
        return {.ok = false, .error = "daemon returned invalid JSON"};
    }
    const bool ok = body.value("ok", false);
    const std::string error = body.value("error", "");
    return {.ok = ok, .body = std::move(body), .error = error};
}

int Client::AttachConsole(const std::string& vm_id) const {
    auto conn = ipc::UnixSocketClient::Connect(socket_path_);
    if (!conn.IsValid()) {
        std::cerr << "failed to connect to " << socket_path_ << "\n";
        return 1;
    }

    if (!WriteJsonLine(conn, {{"type", "vm.console.attach"}, {"vm_id", vm_id}})) {
        std::cerr << "failed to send console attach request\n";
        return 1;
    }

    std::string first = conn.ReadLine();
    auto response = nlohmann::json::parse(first, nullptr, false);
    if (response.is_discarded() || !response.value("ok", false)) {
        std::cerr << (response.is_object() ? response.value("error", "console attach failed")
                                           : "console attach failed")
                  << "\n";
        return 1;
    }

    std::cerr << "Attached to " << vm_id << ". Press Ctrl-] to detach.\n";
    RawTerminal raw;

    while (true) {
        while (conn.HasBufferedLine()) {
            auto event = nlohmann::json::parse(conn.ReadLine(), nullptr, false);
            if (!event.is_discarded()) {
                if (event.value("type", "") == "console.closed") {
                    PrintDetached(raw);
                    return 0;
                }
                HandleConsoleEvent(event);
            }
        }

        pollfd fds[2] = {
            {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
            {.fd = conn.fd(), .events = POLLIN, .revents = 0},
        };
        const int rc = ::poll(fds, 2, -1);
        if (rc <= 0) continue;

        if (fds[0].revents & POLLIN) {
            char buf[4096];
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            if (n == 1 && buf[0] == 0x1d) break;  // Ctrl-]
            const std::string input = StripHostTerminalResponses(buf, static_cast<size_t>(n));
            if (input.empty()) continue;
            if (!WriteJsonLine(conn, {
                    {"type", "console.input"},
                    {"vm_id", vm_id},
                    {"data_hex", BytesToHex(input.data(), input.size())},
                })) {
                break;
            }
        }

        if (fds[1].revents & POLLIN) {
            std::string line = conn.ReadLine();
            if (line.empty()) break;
            auto event = nlohmann::json::parse(line, nullptr, false);
            if (event.is_discarded()) continue;
            if (event.value("type", "") == "console.closed") {
                break;
            }
            HandleConsoleEvent(event);
        }
    }

    PrintDetached(raw);
    return 0;
}

int Client::FollowLogs(const std::string& vm_id) const {
    auto conn = ipc::UnixSocketClient::Connect(socket_path_);
    if (!conn.IsValid()) {
        std::cerr << "failed to connect to " << socket_path_ << "\n";
        return 1;
    }
    if (!WriteJsonLine(conn, {{"type", "vm.logs.follow"}, {"vm_id", vm_id}})) {
        std::cerr << "failed to send logs follow request\n";
        return 1;
    }
    const std::string first = conn.ReadLine();
    auto response = nlohmann::json::parse(first, nullptr, false);
    if (response.is_discarded() || !response.value("ok", false)) {
        std::cerr << (response.is_object() ? response.value("error", "logs follow failed")
                                           : "logs follow failed")
                  << "\n";
        return 1;
    }
    std::cerr << "Following logs for " << vm_id << ". Press Ctrl-C to stop.\n";
    while (true) {
        const std::string line = conn.ReadLine();
        if (line.empty()) break;
        auto event = nlohmann::json::parse(line, nullptr, false);
        if (event.is_discarded()) continue;
        if (event.value("type", "") != "vm.logs.append") continue;
        const auto& payload = event["payload"];
        if (!payload.contains("lines") || !payload["lines"].is_array()) continue;
        for (const auto& l : payload["lines"]) {
            if (!l.is_string()) continue;
            const std::string& s = l.get_ref<const std::string&>();
            ::write(STDOUT_FILENO, s.data(), s.size());
            ::write(STDOUT_FILENO, "\n", 1);
        }
    }
    return 0;
}

}  // namespace tenbox::client
