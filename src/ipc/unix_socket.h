#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>

namespace ipc {

// Unix Domain Socket transport for the AgentSphere IPC protocol.
// Provides bidirectional byte-stream communication between the
// Manager (server) and Runtime (client) processes on macOS/Linux.
//
// Usage:
//   Server side (Manager): UnixSocketServer::Listen() -> Accept()
//   Client side (Runtime): UnixSocketClient::Connect()
//
// Both return a UnixSocketConnection that can Send/Recv.

class UnixSocketConnection {
public:
    explicit UnixSocketConnection(int fd);
    ~UnixSocketConnection();

    // Non-copyable, movable
    UnixSocketConnection(const UnixSocketConnection&) = delete;
    UnixSocketConnection& operator=(const UnixSocketConnection&) = delete;
    UnixSocketConnection(UnixSocketConnection&& other) noexcept;
    UnixSocketConnection& operator=(UnixSocketConnection&& other) noexcept;

    bool IsValid() const { return fd_ >= 0; }

    // Send raw bytes. Returns true on success.
    bool Send(const void* data, size_t len);

    // Send a string (convenience wrapper).
    bool Send(const std::string& data);

    // Receive up to |max_len| bytes. Returns number of bytes read, 0 on EOF, -1 on error.
    ssize_t Recv(void* buf, size_t max_len);

    // Read a complete line (terminated by '\n'). Returns empty on EOF/error.
    std::string ReadLine();

    bool HasBufferedLine() const;

    // Read exactly |len| bytes. Returns false on EOF/error.
    bool ReadExact(void* buf, size_t len);

    void Close();

    int fd() const { return fd_; }

private:
    int fd_ = -1;
    std::string line_buffer_;
    std::mutex send_mutex_;
};

class UnixSocketServer {
public:
    UnixSocketServer() = default;
    ~UnixSocketServer();

    // Start listening on |path|. Removes any existing socket file.
    bool Listen(const std::string& path);

    // Accept a single client connection. Blocks until a client connects.
    UnixSocketConnection Accept();

    void Close();

    std::string path() const { return path_; }

private:
    int listen_fd_ = -1;
    std::string path_;
    // Inode of the socket file we created in Listen(). Captured so Close()
    // can refuse to unlink the path if a *different* listener has rebound
    // the same name in the meantime (this happens on guest reboot, where
    // the new RuntimeSession Listen()s on the same per-vm path while the
    // old session's destructor is still racing to clean up).
    uint64_t bound_inode_ = 0;
};

class UnixSocketClient {
public:
    // Connect to the server at |path|. Returns an invalid connection on failure.
    static UnixSocketConnection Connect(const std::string& path);
};

// Generate a socket path for a given VM ID.
std::string GetSocketPath(const std::string& vm_id);

} // namespace ipc
