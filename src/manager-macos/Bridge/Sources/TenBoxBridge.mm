#import "TenBoxBridge.h"
#include "ipc/unix_socket.h"
#include "ipc/protocol_v1.h"
#include <string>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <mutex>
#include <thread>

static std::mutex g_server_mutex;
static std::unordered_map<std::string, std::unique_ptr<ipc::UnixSocketServer>> g_servers;
static std::unordered_map<std::string, std::unique_ptr<ipc::UnixSocketConnection>> g_accepted;
static std::unordered_map<std::string, std::thread> g_accept_threads;
static std::unordered_map<std::string, NSTask*> g_tasks;

// Ensure all background accept threads are detached before global destructors
// run.  A joinable std::thread that is destructed calls std::terminate.
__attribute__((destructor))
static void CleanupAcceptThreads() {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    for (auto& [_, t] : g_accept_threads) {
        if (t.joinable()) t.detach();
    }
    g_accept_threads.clear();
}

static bool SendControlCommand(const std::string& vmIdStr, const std::string& command) {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    auto it = g_accepted.find(vmIdStr);
    if (it == g_accepted.end() || !it->second || !it->second->IsValid()) {
        return false;
    }

    ipc::Message msg;
    msg.channel = ipc::Channel::kControl;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "runtime.command";
    msg.fields["command"] = command;

    return it->second->Send(ipc::Encode(msg));
}

// File-based VM configuration storage in ~/Library/Application Support/TenBox/
static NSString* GetAppSupportDir() {
    NSArray* paths = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString* appSupport = [paths firstObject];
    NSString* tenboxDir = [appSupport stringByAppendingPathComponent:@"TenBox"];

    NSFileManager* fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:tenboxDir]) {
        [fm createDirectoryAtPath:tenboxDir
      withIntermediateDirectories:YES
                       attributes:nil
                            error:nil];
    }
    return tenboxDir;
}

static NSString* GetVmsDir() {
    NSString* dir = [GetAppSupportDir() stringByAppendingPathComponent:@"vms"];
    NSFileManager* fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:dir]) {
        [fm createDirectoryAtPath:dir
      withIntermediateDirectories:YES
                       attributes:nil
                            error:nil];
    }
    return dir;
}

// ── TBVmInfo ────────────────────────────────────────────────────────

@implementation TBVmInfo
@end

// ── TBVmCreateConfig ────────────────────────────────────────────────

@implementation TBVmCreateConfig
@end

// ── TBBridge ────────────────────────────────────────────────────────

@implementation TBBridge

- (NSArray<TBVmInfo *> *)getVmList {
    NSMutableArray<TBVmInfo *>* result = [NSMutableArray array];
    NSString* vmsDir = GetVmsDir();
    NSFileManager* fm = [NSFileManager defaultManager];

    NSArray* contents = [fm contentsOfDirectoryAtPath:vmsDir error:nil];
    for (NSString* item in contents) {
        NSString* configPath = [[vmsDir stringByAppendingPathComponent:item]
                                stringByAppendingPathComponent:@"config.json"];
        if (![fm fileExistsAtPath:configPath]) continue;

        NSData* data = [NSData dataWithContentsOfFile:configPath];
        if (!data) continue;

        NSDictionary* dict = [NSJSONSerialization JSONObjectWithData:data
                                                             options:0
                                                               error:nil];
        if (!dict) continue;

        NSString* state = dict[@"state"] ?: @"stopped";

        // Reset stale "running" state — if this VM has no active accept
        // thread it cannot be running in this process session.
        if ([state isEqualToString:@"running"]) {
            std::string vmIdStr = item.UTF8String;
            std::lock_guard<std::mutex> lock(g_server_mutex);
            if (g_servers.find(vmIdStr) == g_servers.end()) {
                state = @"stopped";
                NSMutableDictionary* updated = [dict mutableCopy];
                updated[@"state"] = @"stopped";
                NSData* newData = [NSJSONSerialization dataWithJSONObject:updated
                                                                 options:NSJSONWritingPrettyPrinted
                                                                   error:nil];
                [newData writeToFile:configPath atomically:YES];
            }
        }

        TBVmInfo* info = [[TBVmInfo alloc] init];
        info.vmId = item;
        info.name = dict[@"name"] ?: item;
        info.kernelPath = dict[@"kernel_path"] ?: @"";
        info.initrdPath = dict[@"initrd_path"] ?: @"";
        info.diskPath = dict[@"disk_path"] ?: @"";
        info.memoryMb = [dict[@"memory_mb"] integerValue] ?: 512;
        info.cpuCount = [dict[@"cpu_count"] integerValue] ?: 2;
        info.state = state;
        info.netLinkUp = [dict[@"net_link_up"] boolValue];
        [result addObject:info];
    }

    return result;
}

- (BOOL)createVmWithConfig:(TBVmCreateConfig *)config {
    NSString* vmId = [[NSUUID UUID] UUIDString];
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];

    NSFileManager* fm = [NSFileManager defaultManager];
    if (![fm createDirectoryAtPath:vmDir
       withIntermediateDirectories:YES
                        attributes:nil
                             error:nil]) {
        return NO;
    }

    NSDictionary* dict = @{
        @"name": config.name ?: @"",
        @"kernel_path": config.kernelPath ?: @"",
        @"initrd_path": config.initrdPath ?: @"",
        @"disk_path": config.diskPath ?: @"",
        @"memory_mb": @(config.memoryMb),
        @"cpu_count": @(config.cpuCount),
        @"cmdline": config.cmdline ?: @"",
        @"net_enabled": @(config.netEnabled),
        @"state": @"stopped",
        @"net_link_up": @NO,
    };

    NSData* data = [NSJSONSerialization dataWithJSONObject:dict
                                                  options:NSJSONWritingPrettyPrinted
                                                    error:nil];
    NSString* path = [vmDir stringByAppendingPathComponent:@"config.json"];
    return [data writeToFile:path atomically:YES];
}

- (BOOL)deleteVmWithId:(NSString *)vmId {
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    return [[NSFileManager defaultManager] removeItemAtPath:vmDir error:nil];
}

- (BOOL)startVmWithId:(NSString *)vmId {
    // Launch tenbox-vm-runtime as a child process
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return NO;

    NSDictionary* config = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (!config) return NO;

    NSString* kernelPath = config[@"kernel_path"];
    if (!kernelPath || kernelPath.length == 0) return NO;

    // Build command line arguments
    NSMutableArray* args = [NSMutableArray array];
    [args addObject:@"--kernel"];
    [args addObject:kernelPath];
    [args addObject:@"--vm-id"];
    [args addObject:vmId];
    [args addObject:@"--interactive"];
    [args addObject:@"off"];

    NSString* diskPath = config[@"disk_path"];
    if (diskPath && diskPath.length > 0) {
        [args addObject:@"--disk"];
        [args addObject:diskPath];
    }

    NSString* initrdPath = config[@"initrd_path"];
    if (initrdPath && initrdPath.length > 0) {
        [args addObject:@"--initrd"];
        [args addObject:initrdPath];
    }

    NSNumber* memoryMb = config[@"memory_mb"];
    if (memoryMb) {
        [args addObject:@"--memory"];
        [args addObject:[memoryMb stringValue]];
    }

    NSNumber* cpuCount = config[@"cpu_count"];
    if (cpuCount) {
        [args addObject:@"--cpus"];
        [args addObject:[cpuCount stringValue]];
    }

    NSString* cmdline = config[@"cmdline"];
    if (cmdline && cmdline.length > 0) {
        [args addObject:@"--cmdline"];
        [args addObject:cmdline];
    }

    NSNumber* netEnabled = config[@"net_enabled"];
    if (netEnabled && [netEnabled boolValue]) {
        [args addObject:@"--net"];
    }

    std::string sockPath = ipc::GetSocketPath(vmId.UTF8String);
    NSString* socketPath = [NSString stringWithUTF8String:sockPath.c_str()];
    [args addObject:@"--control-endpoint"];
    [args addObject:socketPath];

    // Create the IPC socket server BEFORE launching the runtime so the
    // runtime process can connect immediately.
    {
        auto server = std::make_unique<ipc::UnixSocketServer>();
        if (!server->Listen(sockPath)) {
            NSLog(@"Failed to listen on IPC socket: %s", sockPath.c_str());
            return NO;
        }
        std::string vmIdStr = vmId.UTF8String;
        ipc::UnixSocketServer* rawServer = server.get();

        std::lock_guard<std::mutex> lock(g_server_mutex);
        g_servers[vmIdStr] = std::move(server);

        // Accept the runtime connection in a background thread so the
        // Display page can later attach to it.
        g_accept_threads[vmIdStr] = std::thread([rawServer, vmIdStr]() {
            auto conn = rawServer->Accept();
            if (conn.IsValid()) {
                std::lock_guard<std::mutex> lock(g_server_mutex);
                g_accepted[vmIdStr] = std::make_unique<ipc::UnixSocketConnection>(std::move(conn));
            }
        });
    }

    // Find the runtime binary: beside the manager, then fall back to build/
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* exeDir = [[[NSBundle mainBundle] executablePath]
                         stringByDeletingLastPathComponent];
    NSString* runtimePath = [exeDir stringByAppendingPathComponent:@"tenbox-vm-runtime"];
    if (![fm fileExistsAtPath:runtimePath]) {
        NSString* srcDir = [exeDir stringByDeletingLastPathComponent];
        while (srcDir.pathComponents.count > 1) {
            NSString* candidate = [srcDir stringByAppendingPathComponent:@"build/tenbox-vm-runtime"];
            if ([fm fileExistsAtPath:candidate]) {
                runtimePath = candidate;
                break;
            }
            srcDir = [srcDir stringByDeletingLastPathComponent];
        }
    }

    NSTask* task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:runtimePath];
    task.arguments = args;
    task.currentDirectoryURL = [NSURL fileURLWithPath:vmDir];

    NSError* error = nil;
    if (![task launchAndReturnError:&error]) {
        NSLog(@"Failed to launch runtime: %@", error);
        std::lock_guard<std::mutex> lock(g_server_mutex);
        g_servers.erase(vmId.UTF8String);
        return NO;
    }

    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        g_tasks[vmId.UTF8String] = task;
    }

    // Update state
    NSMutableDictionary* updated = [config mutableCopy];
    updated[@"state"] = @"running";
    NSData* newData = [NSJSONSerialization dataWithJSONObject:updated
                                                     options:NSJSONWritingPrettyPrinted
                                                       error:nil];
    [newData writeToFile:configPath atomically:YES];

    NSString* capturedVmId = [vmId copy];
    NSString* capturedConfigPath = [configPath copy];
    task.terminationHandler = ^(NSTask* t) {
        std::string vmIdStr = capturedVmId.UTF8String;
        {
            std::lock_guard<std::mutex> lock(g_server_mutex);
            g_tasks.erase(vmIdStr);
            g_accepted.erase(vmIdStr);
            g_servers.erase(vmIdStr);
            auto it = g_accept_threads.find(vmIdStr);
            if (it != g_accept_threads.end()) {
                if (it->second.joinable()) it->second.detach();
                g_accept_threads.erase(it);
            }
        }
        NSData* data = [NSData dataWithContentsOfFile:capturedConfigPath];
        if (data) {
            NSMutableDictionary* cfg = [[NSJSONSerialization JSONObjectWithData:data
                                                                       options:NSJSONReadingMutableContainers
                                                                         error:nil] mutableCopy];
            if (cfg) {
                cfg[@"state"] = (t.terminationStatus == 0) ? @"stopped" : @"crashed";
                NSData* out = [NSJSONSerialization dataWithJSONObject:cfg
                                                             options:NSJSONWritingPrettyPrinted
                                                               error:nil];
                [out writeToFile:capturedConfigPath atomically:YES];
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter]
                postNotificationName:@"TenBoxVmStateChanged"
                              object:capturedVmId];
        });
    };

    return YES;
}

- (BOOL)stopVmWithId:(NSString *)vmId {
    {
        std::string vmIdStr = vmId.UTF8String;
        SendControlCommand(vmIdStr, "stop");

        std::lock_guard<std::mutex> lock(g_server_mutex);
        g_accepted.erase(vmIdStr);
        g_servers.erase(vmIdStr);
        auto taskIt = g_tasks.find(vmIdStr);
        if (taskIt != g_tasks.end()) {
            NSTask* task = taskIt->second;
            if (task.isRunning) {
                [task terminate];
            }
            g_tasks.erase(taskIt);
        }
        auto it = g_accept_threads.find(vmIdStr);
        if (it != g_accept_threads.end()) {
            if (it->second.joinable()) it->second.detach();
            g_accept_threads.erase(it);
        }
    }
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return NO;

    NSMutableDictionary* config = [[NSJSONSerialization JSONObjectWithData:data
                                                                  options:NSJSONReadingMutableContainers
                                                                    error:nil] mutableCopy];
    config[@"state"] = @"stopped";
    NSData* newData = [NSJSONSerialization dataWithJSONObject:config
                                                     options:NSJSONWritingPrettyPrinted
                                                       error:nil];
    [newData writeToFile:configPath atomically:YES];
    return YES;
}

- (BOOL)waitForRuntimeConnection:(NSString *)vmId timeout:(NSTimeInterval)timeout {
    std::string vmIdStr = vmId.UTF8String;
    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        auto it = g_accept_threads.find(vmIdStr);
        if (it == g_accept_threads.end()) return NO;
    }

    // Wait for the accept thread to finish (runtime connected)
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

- (void)rebootVmWithId:(NSString *)vmId {
    SendControlCommand(vmId.UTF8String, "reboot");
}

- (void)shutdownVmWithId:(NSString *)vmId {
    SendControlCommand(vmId.UTF8String, "shutdown");
}

@end
