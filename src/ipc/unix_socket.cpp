#include "ipc/unix_socket.h"
#include "core/vmm/types.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace ipc {

// ── UnixSocketConnection ────────────────────────────────────────────

UnixSocketConnection::UnixSocketConnection(int fd) : fd_(fd) {}

UnixSocketConnection::~UnixSocketConnection() {
    Close();
}

UnixSocketConnection::UnixSocketConnection(UnixSocketConnection&& other) noexcept
    : fd_(other.fd_), line_buffer_(std::move(other.line_buffer_)) {
    other.fd_ = -1;
}

UnixSocketConnection& UnixSocketConnection::operator=(UnixSocketConnection&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        line_buffer_ = std::move(other.line_buffer_);
        other.fd_ = -1;
    }
    return *this;
}

bool UnixSocketConnection::Send(const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = ::write(fd_, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool UnixSocketConnection::Send(const std::string& data) {
    return Send(data.data(), data.size());
}

ssize_t UnixSocketConnection::Recv(void* buf, size_t max_len) {
    ssize_t n = ::read(fd_, buf, max_len);
    if (n < 0 && errno == EINTR) return 0;
    return n;
}

std::string UnixSocketConnection::ReadLine() {
    while (true) {
        auto pos = line_buffer_.find('\n');
        if (pos != std::string::npos) {
            std::string line = line_buffer_.substr(0, pos);
            line_buffer_.erase(0, pos + 1);
            return line;
        }

        char buf[65536];
        ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            if (!line_buffer_.empty()) {
                std::string line = std::move(line_buffer_);
                line_buffer_.clear();
                return line;
            }
            return {};
        }
        line_buffer_.append(buf, static_cast<size_t>(n));
    }
}

bool UnixSocketConnection::HasBufferedLine() const {
    return line_buffer_.find('\n') != std::string::npos;
}

bool UnixSocketConnection::ReadExact(void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t remaining = len;

    // Drain line_buffer_ first (may contain leftover data from ReadLine)
    if (!line_buffer_.empty()) {
        size_t avail = std::min(remaining, line_buffer_.size());
        memcpy(p, line_buffer_.data(), avail);
        line_buffer_.erase(0, avail);
        p += avail;
        remaining -= avail;
    }

    while (remaining > 0) {
        ssize_t n = ::read(fd_, p, remaining);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void UnixSocketConnection::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ── UnixSocketServer ────────────────────────────────────────────────

UnixSocketServer::~UnixSocketServer() {
    Close();
}

bool UnixSocketServer::Listen(const std::string& path) {
    // Remove any existing socket file
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("ipc: socket() failed: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        LOG_ERROR("ipc: socket path too long: %s", path.c_str());
        ::close(fd);
        return false;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("ipc: bind(%s) failed: %s", path.c_str(), strerror(errno));
        ::close(fd);
        return false;
    }

    if (::listen(fd, 1) < 0) {
        LOG_ERROR("ipc: listen() failed: %s", strerror(errno));
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) == 0) {
        bound_inode_ = static_cast<uint64_t>(st.st_ino);
    } else {
        bound_inode_ = 0;
    }

    listen_fd_ = fd;
    path_ = path;
    LOG_INFO("ipc: listening on %s", path.c_str());
    return true;
}

UnixSocketConnection UnixSocketServer::Accept() {
    int client_fd = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) {
        LOG_ERROR("ipc: accept() failed: %s", strerror(errno));
        return UnixSocketConnection(-1);
    }
    LOG_DEBUG("ipc: client connected");
    return UnixSocketConnection(client_fd);
}

void UnixSocketServer::Close() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (!path_.empty()) {
        // Only unlink the path if it still names the socket file we created
        // in Listen(). If a different listener has rebound the same path
        // (e.g. a fresh RuntimeSession after a guest reboot reusing the
        // per-vm socket name), unlinking would yank the new listener's
        // socket and break clients trying to connect.
        struct stat st{};
        if (bound_inode_ != 0 &&
            ::stat(path_.c_str(), &st) == 0 &&
            static_cast<uint64_t>(st.st_ino) == bound_inode_) {
            ::unlink(path_.c_str());
        }
        path_.clear();
        bound_inode_ = 0;
    }
}

// ── UnixSocketClient ────────────────────────────────────────────────

UnixSocketConnection UnixSocketClient::Connect(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("ipc: socket() failed: %s", strerror(errno));
        return UnixSocketConnection(-1);
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        LOG_ERROR("ipc: socket path too long: %s", path.c_str());
        ::close(fd);
        return UnixSocketConnection(-1);
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("ipc: connect(%s) failed: %s", path.c_str(), strerror(errno));
        ::close(fd);
        return UnixSocketConnection(-1);
    }

    LOG_DEBUG("ipc: connected to %s", path.c_str());
    return UnixSocketConnection(fd);
}

// ── Utility ─────────────────────────────────────────────────────────

std::string GetSocketPath(const std::string& vm_id) {
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    return std::string(tmpdir) + "/tenbox_vm_" + vm_id + ".sock";
}

} // namespace ipc
