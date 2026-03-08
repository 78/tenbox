#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface TBIpcClient : NSObject

- (BOOL)connectToVm:(NSString *)vmId;
- (BOOL)attachToFd:(int)fd;
- (void)disconnect;
- (BOOL)isConnected;

// Control commands: "stop", "shutdown", "reboot"
- (BOOL)sendControlCommand:(NSString *)command;

// Input events (forwarded to virtio-input)
- (BOOL)sendKeyEvent:(uint16_t)code pressed:(BOOL)pressed;
- (BOOL)sendPointerAbsolute:(int32_t)x y:(int32_t)y buttons:(uint32_t)buttons;
- (BOOL)sendScrollEvent:(int32_t)delta;

// Console input (hex-encoded)
- (BOOL)sendConsoleInput:(NSString *)text;

// Display size hint
- (BOOL)sendDisplaySetSizeWidth:(uint32_t)width height:(uint32_t)height;

// Hot-update shared folders on a running VM.
// Each entry: "tag|host_path|readonly(0/1)"
- (BOOL)sendSharedFoldersUpdate:(NSArray<NSString *> *)entries;

// Hot-update port forwards on a running VM.
// Each entry: "host_port:guest_port" (e.g. "8080:80")
- (BOOL)sendPortForwardsUpdate:(NSArray<NSString *> *)entries netEnabled:(BOOL)netEnabled;

// Clipboard (data_type: 1=UTF8_TEXT, 2=IMAGE_PNG, 3=IMAGE_BMP)
- (BOOL)sendClipboardGrab:(NSArray<NSNumber *> *)types;
- (BOOL)sendClipboardData:(uint32_t)dataType payload:(NSData *)payload;
- (BOOL)sendClipboardRequest:(uint32_t)dataType;
- (BOOL)sendClipboardRelease;

// Start receive loop on background thread; calls blocks on main queue.
// Frame handler: dirtyX/dirtyY = position within the full resource (resW x resH).
//   w/h/stride describe the dirty rectangle payload only.
- (void)startReceiveLoopWithFrameHandler:(void (^)(NSData *pixels, uint32_t w, uint32_t h, uint32_t stride, uint32_t resW, uint32_t resH, uint32_t dirtyX, uint32_t dirtyY))frameHandler
                           cursorHandler:(void (^)(BOOL visible, BOOL imageUpdated, uint32_t width, uint32_t height, uint32_t hotX, uint32_t hotY, NSData * _Nullable pixels))cursorHandler
                            audioHandler:(void (^)(NSData *pcm, uint32_t rate, uint16_t channels))audioHandler
                         consoleHandler:(void (^)(NSString *text))consoleHandler
                    clipboardGrabHandler:(void (^)(NSArray<NSNumber *> *types))clipboardGrabHandler
                    clipboardDataHandler:(void (^)(uint32_t dataType, NSData *payload))clipboardDataHandler
                 clipboardRequestHandler:(void (^)(uint32_t dataType))clipboardRequestHandler
                    runtimeStateHandler:(void (^)(NSString *state))runtimeStateHandler
                  guestAgentStateHandler:(void (^)(BOOL connected))guestAgentStateHandler
                    displayStateHandler:(void (^)(BOOL active, uint32_t width, uint32_t height))displayStateHandler
                       disconnectHandler:(void (^)(void))disconnectHandler;
- (void)stopReceiveLoop;

@end

NS_ASSUME_NONNULL_END
