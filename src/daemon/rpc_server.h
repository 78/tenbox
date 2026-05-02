#pragma once

#include "daemon/kvm_doctor.h"
#include "daemon/remote_session.h"
#include "daemon/runtime_manager.h"
#include "daemon/vm_store.h"
#include "ipc/unix_socket.h"

#include <atomic>
#include <string>
#include <thread>

namespace tenbox::daemon {

class RpcServer {
public:
    RpcServer(DaemonConfig config, VmStore& store, RuntimeManager& runtime_manager);
    ~RpcServer();

    bool Start(std::string* error);
    void Run();
    void Stop();

private:
    void HandleClient(ipc::UnixSocketConnection client);
    nlohmann::json HandleRequest(const nlohmann::json& request);
    void ApplySocketPermissions();
    nlohmann::json CreateVm(const nlohmann::json& request);
    nlohmann::json EditVm(const nlohmann::json& request);
    nlohmann::json VmResources(const VmRecord& record) const;

    DaemonConfig config_;
    VmStore& store_;
    RuntimeManager& runtime_manager_;
    RemoteSessionRegistry remote_sessions_;
    ipc::UnixSocketServer server_;
    std::atomic<bool> running_{false};
};

}  // namespace tenbox::daemon
