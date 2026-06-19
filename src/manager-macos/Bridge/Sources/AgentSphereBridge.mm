#import "AgentSphereBridge.h"
#include "ipc/unix_socket.h"
#include "ipc/protocol_v1.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>

static std::mutex g_server_mutex;
static std::unordered_map<std::string, std::shared_ptr<ipc::UnixSocketServer>> g_servers;
static std::unordered_map<std::string, std::unique_ptr<ipc::UnixSocketConnection>> g_accepted;
static std::unordered_map<std::string, std::thread> g_accept_threads;

__attribute__((destructor))
static void CleanupOnExit() {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    for (auto& [_, t] : g_accept_threads) {
        if (t.joinable()) t.detach();
    }
    g_accept_threads.clear();
    for (auto& [_, srv] : g_servers) {
        srv->Close();
    }
    g_servers.clear();
    g_accepted.clear();
}

// ── TBIpcServer ──────────────────────────────────────────────────

@implementation TBIpcServer

+ (NSString *)socketPathForVm:(NSString *)vmId {
    std::string path = ipc::GetSocketPath(vmId.UTF8String);
    return [NSString stringWithUTF8String:path.c_str()];
}

- (BOOL)listenForVm:(NSString *)vmId socketPath:(NSString *)path {
    std::string sockPath = path.UTF8String;
    auto server = std::make_shared<ipc::UnixSocketServer>();
    if (!server->Listen(sockPath)) {
        NSLog(@"Failed to listen on IPC socket: %s", sockPath.c_str());
        return NO;
    }

    std::string vmIdStr = vmId.UTF8String;
    std::lock_guard<std::mutex> lock(g_server_mutex);
    g_servers[vmIdStr] = server;
    g_accept_threads[vmIdStr] = std::thread([server, vmIdStr]() {
        auto conn = server->Accept();
        if (conn.IsValid()) {
            std::lock_guard<std::mutex> lock(g_server_mutex);
            g_accepted[vmIdStr] = std::make_unique<ipc::UnixSocketConnection>(std::move(conn));
        }
    });
    return YES;
}

- (BOOL)isServerActiveForVm:(NSString *)vmId {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_servers.find(vmId.UTF8String) != g_servers.end();
}

- (BOOL)sendControlCommand:(NSString *)command toVm:(NSString *)vmId {
    std::string vmIdStr = vmId.UTF8String;
    std::lock_guard<std::mutex> lock(g_server_mutex);
    auto it = g_accepted.find(vmIdStr);
    if (it == g_accepted.end() || !it->second || !it->second->IsValid()) {
        return NO;
    }
    ipc::Message msg;
    msg.channel = ipc::Channel::kControl;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "runtime.command";
    msg.fields["command"] = command.UTF8String;
    return it->second->Send(ipc::Encode(msg));
}

- (BOOL)sendSharedFoldersUpdate:(NSArray<NSString *> *)entries toVm:(NSString *)vmId {
    std::string vmIdStr = vmId.UTF8String;
    std::lock_guard<std::mutex> lock(g_server_mutex);
    auto it = g_accepted.find(vmIdStr);
    if (it == g_accepted.end() || !it->second || !it->second->IsValid()) {
        return NO;
    }
    ipc::Message msg;
    msg.channel = ipc::Channel::kControl;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "runtime.update_shared_folders";
    msg.fields["folder_count"] = std::to_string(entries.count);
    for (NSUInteger i = 0; i < entries.count; ++i) {
        msg.fields["folder_" + std::to_string(i)] = entries[i].UTF8String;
    }
    return it->second->Send(ipc::Encode(msg));
}

- (BOOL)waitForConnection:(NSString *)vmId timeout:(NSTimeInterval)timeout {
    std::string vmIdStr = vmId.UTF8String;
    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        if (g_accept_threads.find(vmIdStr) == g_accept_threads.end()) return NO;
    }
    auto deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    while ([deadline timeIntervalSinceNow] > 0) {
        {
            std::lock_guard<std::mutex> lock(g_server_mutex);
            auto it = g_accepted.find(vmIdStr);
            if (it != g_accepted.end() && it->second && it->second->IsValid()) {
                return YES;
            }
        }
        [NSThread sleepForTimeInterval:0.1];
    }
    return NO;
}

- (int)takeAcceptedFdForVm:(NSString *)vmId {
    std::string vmIdStr = vmId.UTF8String;
    std::lock_guard<std::mutex> lock(g_server_mutex);
    auto it = g_accepted.find(vmIdStr);
    if (it == g_accepted.end() || !it->second || !it->second->IsValid()) {
        return -1;
    }
    return it->second->TakeFd();
}

- (void)closeResourcesForVm:(NSString *)vmId {
    std::string vmIdStr = vmId.UTF8String;
    std::lock_guard<std::mutex> lock(g_server_mutex);
    g_accepted.erase(vmIdStr);
    auto si = g_servers.find(vmIdStr);
    if (si != g_servers.end()) {
        si->second->Close();
        g_servers.erase(si);
    }
    auto it = g_accept_threads.find(vmIdStr);
    if (it != g_accept_threads.end()) {
        if (it->second.joinable()) it->second.detach();
        g_accept_threads.erase(it);
    }
}

- (void)closeAllResources {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    g_accepted.clear();
    for (auto& [_, srv] : g_servers) {
        srv->Close();
    }
    g_servers.clear();
    for (auto& [_, t] : g_accept_threads) {
        if (t.joinable()) t.detach();
    }
    g_accept_threads.clear();
}

@end
