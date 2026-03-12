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
static std::unordered_map<std::string, NSMutableArray<NSURL *> *> g_scoped_urls;

// Last-resort cleanup: terminate any remaining child processes and detach
// accept threads before global destructors run.
__attribute__((destructor))
static void CleanupOnExit() {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    for (auto& [_, task] : g_tasks) {
        if (task.isRunning) {
            [task terminate];
        }
    }
    g_tasks.clear();
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

// ── TBPortForward ──────────────────────────────────────────────────

@implementation TBPortForward
@end

// ── TBSharedFolder ──────────────────────────────────────────────────

@implementation TBSharedFolder
@end

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
        info.netEnabled = [dict[@"net_enabled"] boolValue];
        // cmdline is handled by the runtime with its built-in default
        NSNumber* displayScaleNum = dict[@"display_scale"];
        info.displayScale = displayScaleNum ? [displayScaleNum integerValue] : 1;
        if (info.displayScale < 1) info.displayScale = 1;
        if (info.displayScale > 2) info.displayScale = 2;

        NSMutableArray<TBSharedFolder *>* folders = [NSMutableArray array];
        NSArray* sfArray = dict[@"shared_folders"];
        if ([sfArray isKindOfClass:[NSArray class]]) {
            for (NSDictionary* sfDict in sfArray) {
                NSString* tag = sfDict[@"tag"];
                NSString* hostPath = sfDict[@"host_path"];
                if (tag && hostPath) {
                    TBSharedFolder* sf = [[TBSharedFolder alloc] init];
                    sf.tag = tag;
                    sf.hostPath = hostPath;
                    sf.readonly_ = [sfDict[@"readonly"] boolValue];
                    NSString* bmBase64 = sfDict[@"bookmark_base64"];
                    if ([bmBase64 isKindOfClass:[NSString class]] && bmBase64.length > 0) {
                        sf.bookmark = [[NSData alloc] initWithBase64EncodedString:bmBase64 options:0];
                    }
                    [folders addObject:sf];
                }
            }
        }
        info.sharedFolders = folders;

        NSMutableArray<TBPortForward *>* pfs = [NSMutableArray array];
        NSArray* pfArray = dict[@"port_forwards"];
        if ([pfArray isKindOfClass:[NSArray class]]) {
            for (NSDictionary* pfDict in pfArray) {
                NSNumber* hp = pfDict[@"host_port"];
                NSNumber* gp = pfDict[@"guest_port"];
                if (hp && gp) {
                    TBPortForward* pf = [[TBPortForward alloc] init];
                    pf.hostPort = [hp unsignedShortValue];
                    pf.guestPort = [gp unsignedShortValue];
                    [pfs addObject:pf];
                }
            }
        }
        info.portForwards = pfs;
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

    NSString* kernelPath = config.kernelPath ?: @"";
    NSString* initrdPath = config.initrdPath ?: @"";
    NSString* diskPath = config.diskPath ?: @"";

    // Copy source files into the VM directory so each VM is self-contained.
    NSString* (^CopyFile)(NSString*) = ^NSString*(NSString* srcPath) {
        if (srcPath.length == 0) return @"";
        NSString* fileName = [srcPath lastPathComponent];
        NSString* dest = [vmDir stringByAppendingPathComponent:fileName];
        NSError* err = nil;
        if ([fm copyItemAtPath:srcPath toPath:dest error:&err]) {
            return dest;
        }
        NSLog(@"Failed to copy %@ -> %@: %@", srcPath, dest, err);
        return srcPath;
    };

    if (kernelPath.length > 0) kernelPath = CopyFile(kernelPath);
    if (initrdPath.length > 0) initrdPath = CopyFile(initrdPath);
    if (diskPath.length > 0) diskPath = CopyFile(diskPath);

    NSDictionary* dict = @{
        @"name": config.name ?: @"",
        @"kernel_path": kernelPath,
        @"initrd_path": initrdPath,
        @"disk_path": diskPath,
        @"memory_mb": @(config.memoryMb),
        @"cpu_count": @(config.cpuCount),
        @"net_enabled": @(config.netEnabled),
        @"state": @"stopped",
        @"display_scale": @1,
        @"shared_folders": @[],
        @"port_forwards": @[],
    };

    NSData* data = [NSJSONSerialization dataWithJSONObject:dict
                                                  options:NSJSONWritingPrettyPrinted
                                                    error:nil];
    NSString* path = [vmDir stringByAppendingPathComponent:@"config.json"];
    return [data writeToFile:path atomically:YES];
}

- (BOOL)editVmWithId:(NSString *)vmId name:(NSString *)name memoryMb:(NSInteger)memoryMb cpuCount:(NSInteger)cpuCount netEnabled:(BOOL)netEnabled {
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return NO;

    NSMutableDictionary* config = [[NSJSONSerialization JSONObjectWithData:data
                                                                  options:NSJSONReadingMutableContainers
                                                                    error:nil] mutableCopy];
    if (!config) return NO;

    config[@"name"] = name;
    config[@"memory_mb"] = @(memoryMb);
    config[@"cpu_count"] = @(cpuCount);
    config[@"net_enabled"] = @(netEnabled);

    NSData* newData = [NSJSONSerialization dataWithJSONObject:config
                                                     options:NSJSONWritingPrettyPrinted
                                                       error:nil];
    return [newData writeToFile:configPath atomically:YES];
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

    NSNumber* netEnabled = config[@"net_enabled"];
    if (netEnabled && [netEnabled boolValue]) {
        [args addObject:@"--net"];
    }

    NSArray* portForwards = config[@"port_forwards"];
    if ([portForwards isKindOfClass:[NSArray class]]) {
        for (NSDictionary* pf in portForwards) {
            NSNumber* hp = pf[@"host_port"];
            NSNumber* gp = pf[@"guest_port"];
            if (hp && gp) {
                [args addObject:@"--forward"];
                [args addObject:[NSString stringWithFormat:@"%u:%u",
                    [hp unsignedShortValue], [gp unsignedShortValue]]];
            }
        }
    }

    NSMutableArray<NSURL *>* scopedUrls = [NSMutableArray array];
    NSArray* sharedFolders = config[@"shared_folders"];
    if ([sharedFolders isKindOfClass:[NSArray class]]) {
        for (NSDictionary* sf in sharedFolders) {
            NSString* tag = sf[@"tag"];
            NSString* hostPath = sf[@"host_path"];
            if (tag.length == 0 || hostPath.length == 0) continue;

            // Resolve security-scoped bookmark to get access to protected dirs
            NSString* bmBase64 = sf[@"bookmark_base64"];
            if ([bmBase64 isKindOfClass:[NSString class]] && bmBase64.length > 0) {
                NSData* bmData = [[NSData alloc] initWithBase64EncodedString:bmBase64 options:0];
                if (bmData) {
                    BOOL stale = NO;
                    NSError* err = nil;
                    NSURL* url = [NSURL URLByResolvingBookmarkData:bmData
                                                           options:NSURLBookmarkResolutionWithSecurityScope
                                                     relativeToURL:nil
                                               bookmarkDataIsStale:&stale
                                                             error:&err];
                    if (url && !err) {
                        if ([url startAccessingSecurityScopedResource]) {
                            [scopedUrls addObject:url];
                            hostPath = url.path;
                        }
                    }
                }
            }

            NSString* arg = [sf[@"readonly"] boolValue]
                ? [NSString stringWithFormat:@"%@:%@:ro", tag, hostPath]
                : [NSString stringWithFormat:@"%@:%@", tag, hostPath];
            [args addObject:@"--share"];
            [args addObject:arg];
        }
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

    NSLog(@"Launch runtime with arguments: %@", args);

    NSError* error = nil;
    if (![task launchAndReturnError:&error]) {
        NSLog(@"Failed to launch runtime: %@", error);
        for (NSURL* url in scopedUrls) {
            [url stopAccessingSecurityScopedResource];
        }
        std::lock_guard<std::mutex> lock(g_server_mutex);
        g_servers.erase(vmId.UTF8String);
        return NO;
    }

    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        g_tasks[vmId.UTF8String] = task;
        if (scopedUrls.count > 0) {
            g_scoped_urls[vmId.UTF8String] = scopedUrls;
        }
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
    __weak TBBridge* weakSelf = self;
    task.terminationHandler = ^(NSTask* t) {
        BOOL wantsReboot = (t.terminationStatus == 128);
        std::string vmIdStr = capturedVmId.UTF8String;
        NSMutableArray<NSURL *>* urlsToRelease = nil;
        {
            std::lock_guard<std::mutex> lock(g_server_mutex);
            g_tasks.erase(vmIdStr);
            g_accepted.erase(vmIdStr);
            g_servers.erase(vmIdStr);
            auto sit = g_scoped_urls.find(vmIdStr);
            if (sit != g_scoped_urls.end()) {
                urlsToRelease = sit->second;
                g_scoped_urls.erase(sit);
            }
            auto it = g_accept_threads.find(vmIdStr);
            if (it != g_accept_threads.end()) {
                if (it->second.joinable()) it->second.detach();
                g_accept_threads.erase(it);
            }
        }
        for (NSURL* url in urlsToRelease) {
            [url stopAccessingSecurityScopedResource];
        }

        if (wantsReboot) {
            NSLog(@"VM %@ exited with code 128 (reboot), restarting...", capturedVmId);
            NSData* data = [NSData dataWithContentsOfFile:capturedConfigPath];
            if (data) {
                NSMutableDictionary* cfg = [[NSJSONSerialization JSONObjectWithData:data
                                                                           options:NSJSONReadingMutableContainers
                                                                             error:nil] mutableCopy];
                if (cfg) {
                    cfg[@"state"] = @"rebooting";
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
            dispatch_async(dispatch_get_main_queue(), ^{
                TBBridge* strongSelf = weakSelf;
                if (strongSelf) {
                    [strongSelf startVmWithId:capturedVmId];
                    [[NSNotificationCenter defaultCenter]
                        postNotificationName:@"TenBoxVmStateChanged"
                                      object:capturedVmId];
                }
            });
            return;
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
    NSMutableArray<NSURL *>* urlsToRelease = nil;
    {
        std::string vmIdStr = vmId.UTF8String;
        SendControlCommand(vmIdStr, "stop");

        std::lock_guard<std::mutex> lock(g_server_mutex);
        g_accepted.erase(vmIdStr);
        g_servers.erase(vmIdStr);
        auto sit = g_scoped_urls.find(vmIdStr);
        if (sit != g_scoped_urls.end()) {
            urlsToRelease = sit->second;
            g_scoped_urls.erase(sit);
        }
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
    for (NSURL* url in urlsToRelease) {
        [url stopAccessingSecurityScopedResource];
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

- (BOOL)setDisplayScale:(NSInteger)scale forVm:(NSString *)vmId {
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return NO;

    NSMutableDictionary* config = [[NSJSONSerialization JSONObjectWithData:data
                                                                  options:NSJSONReadingMutableContainers
                                                                    error:nil] mutableCopy];
    if (!config) return NO;

    NSInteger clamped = MAX(1, MIN(2, scale));
    config[@"display_scale"] = @(clamped);

    NSData* newData = [NSJSONSerialization dataWithJSONObject:config
                                                     options:NSJSONWritingPrettyPrinted
                                                       error:nil];
    return [newData writeToFile:configPath atomically:YES];
}

// ── Shared folder helpers ─────────────────────────────────────────

static NSDictionary* SharedFolderToJson(TBSharedFolder* sf) {
    NSMutableDictionary* d = [NSMutableDictionary dictionaryWithDictionary:@{
        @"tag": sf.tag ?: @"",
        @"host_path": sf.hostPath ?: @"",
        @"readonly": @(sf.readonly_),
    }];
    if (sf.bookmark) {
        d[@"bookmark_base64"] = [sf.bookmark base64EncodedStringWithOptions:0];
    }
    return d;
}

static NSArray* SharedFoldersToJson(NSArray<TBSharedFolder *>* folders) {
    NSMutableArray* arr = [NSMutableArray arrayWithCapacity:folders.count];
    for (TBSharedFolder* sf in folders) {
        [arr addObject:SharedFolderToJson(sf)];
    }
    return arr;
}

static void SendSharedFoldersUpdate(const std::string& vmIdStr,
                                    NSArray<TBSharedFolder *>* folders) {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    auto it = g_accepted.find(vmIdStr);
    if (it == g_accepted.end() || !it->second || !it->second->IsValid()) return;

    ipc::Message msg;
    msg.channel = ipc::Channel::kControl;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "runtime.update_shared_folders";
    msg.fields["folder_count"] = std::to_string(folders.count);
    for (NSUInteger i = 0; i < folders.count; ++i) {
        TBSharedFolder* f = folders[i];
        std::string val = std::string(f.tag.UTF8String) + "|" +
                          std::string(f.hostPath.UTF8String) + "|" +
                          (f.readonly_ ? "1" : "0");
        msg.fields["folder_" + std::to_string(i)] = val;
    }
    it->second->Send(ipc::Encode(msg));
}

- (BOOL)addSharedFolder:(TBSharedFolder *)folder toVm:(NSString *)vmId {
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return NO;

    NSMutableDictionary* config = [[NSJSONSerialization JSONObjectWithData:data
                                                                  options:NSJSONReadingMutableContainers
                                                                    error:nil] mutableCopy];
    if (!config) return NO;

    NSMutableArray* sfArray = [NSMutableArray arrayWithArray:config[@"shared_folders"] ?: @[]];
    for (NSDictionary* existing in sfArray) {
        if ([existing[@"tag"] isEqualToString:folder.tag]) return NO;
    }
    [sfArray addObject:SharedFolderToJson(folder)];
    config[@"shared_folders"] = sfArray;

    NSData* newData = [NSJSONSerialization dataWithJSONObject:config
                                                     options:NSJSONWritingPrettyPrinted
                                                       error:nil];
    if (![newData writeToFile:configPath atomically:YES]) return NO;

    // Hot-update if running
    NSMutableArray<TBSharedFolder *>* allFolders = [NSMutableArray array];
    for (NSDictionary* d in sfArray) {
        TBSharedFolder* sf = [[TBSharedFolder alloc] init];
        sf.tag = d[@"tag"];
        sf.hostPath = d[@"host_path"];
        sf.readonly_ = [d[@"readonly"] boolValue];
        [allFolders addObject:sf];
    }
    SendSharedFoldersUpdate(vmId.UTF8String, allFolders);
    return YES;
}

- (BOOL)removeSharedFolderWithTag:(NSString *)tag fromVm:(NSString *)vmId {
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return NO;

    NSMutableDictionary* config = [[NSJSONSerialization JSONObjectWithData:data
                                                                  options:NSJSONReadingMutableContainers
                                                                    error:nil] mutableCopy];
    if (!config) return NO;

    NSMutableArray* sfArray = [NSMutableArray arrayWithArray:config[@"shared_folders"] ?: @[]];
    NSUInteger idx = NSNotFound;
    for (NSUInteger i = 0; i < sfArray.count; ++i) {
        if ([sfArray[i][@"tag"] isEqualToString:tag]) { idx = i; break; }
    }
    if (idx == NSNotFound) return NO;
    [sfArray removeObjectAtIndex:idx];
    config[@"shared_folders"] = sfArray;

    NSData* newData = [NSJSONSerialization dataWithJSONObject:config
                                                     options:NSJSONWritingPrettyPrinted
                                                       error:nil];
    if (![newData writeToFile:configPath atomically:YES]) return NO;

    NSMutableArray<TBSharedFolder *>* allFolders = [NSMutableArray array];
    for (NSDictionary* d in sfArray) {
        TBSharedFolder* sf = [[TBSharedFolder alloc] init];
        sf.tag = d[@"tag"];
        sf.hostPath = d[@"host_path"];
        sf.readonly_ = [d[@"readonly"] boolValue];
        [allFolders addObject:sf];
    }
    SendSharedFoldersUpdate(vmId.UTF8String, allFolders);
    return YES;
}

- (BOOL)setSharedFolders:(NSArray<TBSharedFolder *> *)folders forVm:(NSString *)vmId {
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return NO;

    NSMutableDictionary* config = [[NSJSONSerialization JSONObjectWithData:data
                                                                  options:NSJSONReadingMutableContainers
                                                                    error:nil] mutableCopy];
    if (!config) return NO;

    config[@"shared_folders"] = SharedFoldersToJson(folders);

    NSData* newData = [NSJSONSerialization dataWithJSONObject:config
                                                     options:NSJSONWritingPrettyPrinted
                                                       error:nil];
    if (![newData writeToFile:configPath atomically:YES]) return NO;

    SendSharedFoldersUpdate(vmId.UTF8String, folders);
    return YES;
}

// ── Port forward helpers ─────────────────────────────────────────

static NSDictionary* PortForwardToJson(TBPortForward* pf) {
    return @{
        @"host_port": @(pf.hostPort),
        @"guest_port": @(pf.guestPort),
    };
}

static void SendPortForwardsUpdate(const std::string& vmIdStr,
                                   NSArray* pfJsonArray,
                                   BOOL netEnabled) {
    std::lock_guard<std::mutex> lock(g_server_mutex);
    auto it = g_accepted.find(vmIdStr);
    if (it == g_accepted.end() || !it->second || !it->second->IsValid()) return;

    ipc::Message msg;
    msg.channel = ipc::Channel::kControl;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "runtime.update_network";
    msg.fields["link_up"] = netEnabled ? "true" : "false";
    msg.fields["forward_count"] = std::to_string(pfJsonArray.count);
    for (NSUInteger i = 0; i < pfJsonArray.count; ++i) {
        NSDictionary* pf = pfJsonArray[i];
        uint16_t hp = [pf[@"host_port"] unsignedShortValue];
        uint16_t gp = [pf[@"guest_port"] unsignedShortValue];
        msg.fields["forward_" + std::to_string(i)] =
            std::to_string(hp) + ":" + std::to_string(gp);
    }
    it->second->Send(ipc::Encode(msg));
}

- (BOOL)addPortForward:(TBPortForward *)pf toVm:(NSString *)vmId {
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return NO;

    NSMutableDictionary* config = [[NSJSONSerialization JSONObjectWithData:data
                                                                  options:NSJSONReadingMutableContainers
                                                                    error:nil] mutableCopy];
    if (!config) return NO;

    NSMutableArray* pfArray = [NSMutableArray arrayWithArray:config[@"port_forwards"] ?: @[]];
    for (NSDictionary* existing in pfArray) {
        if ([existing[@"host_port"] unsignedShortValue] == pf.hostPort) return NO;
    }
    [pfArray addObject:PortForwardToJson(pf)];
    config[@"port_forwards"] = pfArray;

    NSData* newData = [NSJSONSerialization dataWithJSONObject:config
                                                     options:NSJSONWritingPrettyPrinted
                                                       error:nil];
    if (![newData writeToFile:configPath atomically:YES]) return NO;

    BOOL netEnabled = [config[@"net_enabled"] boolValue];
    SendPortForwardsUpdate(vmId.UTF8String, pfArray, netEnabled);
    return YES;
}

- (BOOL)removePortForwardWithHostPort:(uint16_t)hostPort fromVm:(NSString *)vmId {
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return NO;

    NSMutableDictionary* config = [[NSJSONSerialization JSONObjectWithData:data
                                                                  options:NSJSONReadingMutableContainers
                                                                    error:nil] mutableCopy];
    if (!config) return NO;

    NSMutableArray* pfArray = [NSMutableArray arrayWithArray:config[@"port_forwards"] ?: @[]];
    NSUInteger idx = NSNotFound;
    for (NSUInteger i = 0; i < pfArray.count; ++i) {
        if ([pfArray[i][@"host_port"] unsignedShortValue] == hostPort) { idx = i; break; }
    }
    if (idx == NSNotFound) return NO;
    [pfArray removeObjectAtIndex:idx];
    config[@"port_forwards"] = pfArray;

    NSData* newData = [NSJSONSerialization dataWithJSONObject:config
                                                     options:NSJSONWritingPrettyPrinted
                                                       error:nil];
    if (![newData writeToFile:configPath atomically:YES]) return NO;

    BOOL netEnabled = [config[@"net_enabled"] boolValue];
    SendPortForwardsUpdate(vmId.UTF8String, pfArray, netEnabled);
    return YES;
}

- (NSArray<TBPortForward *> *)getPortForwards:(NSString *)vmId {
    NSString* vmDir = [GetVmsDir() stringByAppendingPathComponent:vmId];
    NSString* configPath = [vmDir stringByAppendingPathComponent:@"config.json"];
    NSData* data = [NSData dataWithContentsOfFile:configPath];
    if (!data) return @[];

    NSDictionary* config = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    NSMutableArray<TBPortForward *>* result = [NSMutableArray array];
    NSArray* pfArray = config[@"port_forwards"];
    if ([pfArray isKindOfClass:[NSArray class]]) {
        for (NSDictionary* d in pfArray) {
            NSNumber* hp = d[@"host_port"];
            NSNumber* gp = d[@"guest_port"];
            if (hp && gp) {
                TBPortForward* pf = [[TBPortForward alloc] init];
                pf.hostPort = [hp unsignedShortValue];
                pf.guestPort = [gp unsignedShortValue];
                [result addObject:pf];
            }
        }
    }
    return result;
}

- (void)stopAllVms {
    // Collect running VM IDs, then send stop commands without holding the lock
    // (SendControlCommand acquires g_server_mutex internally).
    std::vector<std::string> runningIds;
    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        for (auto& [vmIdStr, _] : g_tasks) {
            runningIds.push_back(vmIdStr);
        }
    }

    for (auto& vmIdStr : runningIds) {
        SendControlCommand(vmIdStr, "stop");
    }

    std::lock_guard<std::mutex> lock(g_server_mutex);
    for (auto& [_, task] : g_tasks) {
        if (task.isRunning) {
            [task terminate];
            [task waitUntilExit];
        }
    }
    g_tasks.clear();
    g_accepted.clear();
    g_servers.clear();
    for (auto& [_, urls] : g_scoped_urls) {
        for (NSURL* url in urls) {
            [url stopAccessingSecurityScopedResource];
        }
    }
    g_scoped_urls.clear();
    for (auto& [_, t] : g_accept_threads) {
        if (t.joinable()) t.detach();
    }
    g_accept_threads.clear();
}

@end
