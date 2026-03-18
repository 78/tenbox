#pragma once

#import <Foundation/Foundation.h>
#import "TenBoxIPC.h"

NS_ASSUME_NONNULL_BEGIN

@interface TBPortForward : NSObject
@property (nonatomic, assign) uint16_t hostPort;
@property (nonatomic, assign) uint16_t guestPort;
@property (nonatomic, copy) NSString *hostIp;
@property (nonatomic, copy) NSString *guestIp;
@end

@interface TBGuestForward : NSObject
@property (nonatomic, copy) NSString *guestIp;
@property (nonatomic, assign) uint16_t guestPort;
@property (nonatomic, copy) NSString *hostAddr;
@property (nonatomic, assign) uint16_t hostPort;
@end

@interface TBSharedFolder : NSObject
@property (nonatomic, copy) NSString *tag;
@property (nonatomic, copy) NSString *hostPath;
@property (nonatomic, assign) BOOL readonly_;
@property (nonatomic, copy, nullable) NSData *bookmark;
@end

@interface TBVmInfo : NSObject
@property (nonatomic, copy) NSString *vmId;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *kernelPath;
@property (nonatomic, copy) NSString *initrdPath;
@property (nonatomic, copy) NSString *diskPath;
@property (nonatomic, assign) NSInteger memoryMb;
@property (nonatomic, assign) NSInteger cpuCount;
@property (nonatomic, copy) NSString *state;
@property (nonatomic, assign) BOOL netEnabled;
@property (nonatomic, copy) NSArray<TBSharedFolder *> *sharedFolders;
@property (nonatomic, copy) NSArray<TBPortForward *> *portForwards;
@property (nonatomic, copy) NSArray<TBGuestForward *> *guestForwards;
@property (nonatomic, assign) NSInteger displayScale;
@property (nonatomic, assign) BOOL debugMode;
@end

@interface TBVmCreateConfig : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *kernelPath;
@property (nonatomic, copy) NSString *initrdPath;
@property (nonatomic, copy) NSString *diskPath;
@property (nonatomic, assign) NSInteger memoryMb;
@property (nonatomic, assign) NSInteger cpuCount;
@property (nonatomic, assign) BOOL netEnabled;
@property (nonatomic, copy) NSString *sourceDir;
@property (nonatomic, assign) BOOL debugMode;
@end

@interface TBBridge : NSObject

- (NSArray<TBVmInfo *> *)getVmList;
- (BOOL)createVmWithConfig:(TBVmCreateConfig *)config;
- (BOOL)editVmWithId:(NSString *)vmId name:(NSString *)name memoryMb:(NSInteger)memoryMb cpuCount:(NSInteger)cpuCount netEnabled:(BOOL)netEnabled debugMode:(BOOL)debugMode;
- (BOOL)deleteVmWithId:(NSString *)vmId;
- (BOOL)startVmWithId:(NSString *)vmId;
- (BOOL)stopVmWithId:(NSString *)vmId;
- (void)rebootVmWithId:(NSString *)vmId;
- (void)shutdownVmWithId:(NSString *)vmId;

// Shared folder management
- (BOOL)addSharedFolder:(TBSharedFolder *)folder toVm:(NSString *)vmId;
- (BOOL)removeSharedFolderWithTag:(NSString *)tag fromVm:(NSString *)vmId;
- (BOOL)setSharedFolders:(NSArray<TBSharedFolder *> *)folders forVm:(NSString *)vmId;

// Display scale management
- (BOOL)setDisplayScale:(NSInteger)scale forVm:(NSString *)vmId;

// Port forward management
- (BOOL)addPortForward:(TBPortForward *)pf toVm:(NSString *)vmId;
- (BOOL)removePortForwardWithHostPort:(uint16_t)hostPort fromVm:(NSString *)vmId;
- (NSArray<TBPortForward *> *)getPortForwards:(NSString *)vmId;

// Guest forward management
- (BOOL)addGuestForward:(TBGuestForward *)gf toVm:(NSString *)vmId;
- (BOOL)removeGuestForwardWithGuestIp:(NSString *)guestIp guestPort:(uint16_t)guestPort fromVm:(NSString *)vmId;

// Terminate all running VM processes. Call on app exit.
- (void)stopAllVms;

// Wait for the runtime process to connect (up to timeout seconds).
- (BOOL)waitForRuntimeConnection:(NSString *)vmId timeout:(NSTimeInterval)timeout;

// Take the accepted socket fd for a VM. Returns -1 if not available.
// Ownership is transferred; caller must close the fd.
- (int)takeAcceptedFdForVm:(NSString *)vmId;

@end

NS_ASSUME_NONNULL_END
