#include "platform/posix/console/posix_console_port.h"
#include "core/vmm/types.h"
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

PosixConsolePort::PosixConsolePort() {
    if (isatty(STDIN_FILENO)) {
        tcgetattr(STDIN_FILENO, &orig_termios_);
        struct termios raw = orig_termios_;
        cfmakeraw(&raw);
        // Preserve ISIG so Ctrl-C still sends SIGINT to the process
        raw.c_lflag |= ISIG;
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;  // 100ms read timeout
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        raw_mode_ = true;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
}

PosixConsolePort::~PosixConsolePort() {
    if (raw_mode_) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_);
    }
}

void PosixConsolePort::Write(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    std::lock_guard<std::mutex> lock(GetStdoutMutex());
    // write(2) may return a short count on a pipe or be interrupted by a
    // signal; loop until all bytes are out (or a hard error is hit) so we
    // don't silently drop guest console output.
    size_t off = 0;
    while (off < size) {
        ssize_t n = ::write(STDOUT_FILENO, data + off, size - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

size_t PosixConsolePort::Read(uint8_t* out, size_t size) {
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, 50);
    if (ret <= 0) return 0;

    ssize_t n = ::read(STDIN_FILENO, out, size);
    return (n > 0) ? static_cast<size_t>(n) : 0;
}
