#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace tenbox::client {

std::string DefaultSocketPath();

struct Response {
    bool ok = false;
    nlohmann::json body;
    std::string error;
};

class Client {
public:
    explicit Client(std::string socket_path = DefaultSocketPath());

    Response Request(const nlohmann::json& request) const;
    int AttachConsole(const std::string& vm_id) const;
    // Stream runtime stdout/stderr until the user hits Ctrl-C or the VM
    // stops. Each `vm.logs.append` event from the daemon is written to
    // STDOUT line-by-line. Returns 0 on clean detach, non-zero on transport
    // / RPC errors.
    int FollowLogs(const std::string& vm_id) const;

    const std::string& socket_path() const { return socket_path_; }

private:
    std::string socket_path_;
};

}  // namespace tenbox::client
