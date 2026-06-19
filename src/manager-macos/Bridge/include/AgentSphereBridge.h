#pragma once

#import <Foundation/Foundation.h>
#import "AgentSphereIPC.h"

NS_ASSUME_NONNULL_BEGIN

@interface TBIpcServer : NSObject

// Create an IPC socket server for a VM and start a background accept thread.
- (BOOL)listenForVm:(NSString *)vmId socketPath:(NSString *)path;

// Check if a VM has an active IPC server (for stale state detection).
- (BOOL)isServerActiveForVm:(NSString *)vmId;

// Get the IPC socket path for a VM.
+ (NSString *)socketPathForVm:(NSString *)vmId;

// Send a control command (stop/shutdown/reboot) to a running VM.
- (BOOL)sendControlCommand:(NSString *)command toVm:(NSString *)vmId;

// Send a shared-folders hot-update to a running VM.
// Each entry: "tag|host_path|readonly(0/1)"
- (BOOL)sendSharedFoldersUpdate:(NSArray<NSString *> *)entries toVm:(NSString *)vmId;

// Wait for the runtime process to connect (up to timeout seconds).
- (BOOL)waitForConnection:(NSString *)vmId timeout:(NSTimeInterval)timeout;

// Take the accepted socket fd for a VM. Returns -1 if not available.
// Ownership is transferred; caller must close the fd.
- (int)takeAcceptedFdForVm:(NSString *)vmId;

// Close IPC resources (socket server, accepted connection, accept thread) for a single VM.
- (void)closeResourcesForVm:(NSString *)vmId;

// Close all IPC resources. Call on app exit.
- (void)closeAllResources;

@end

NS_ASSUME_NONNULL_END
