#import "TenBoxIPC.h"
#include "ipc/unix_socket.h"
#include "ipc/protocol_v1.h"
#include "ipc/shared_framebuffer.h"
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <unistd.h>
#include <chrono>
#include <iomanip>
#include <sstream>

static const bool kDebugIpcMessages = false;

#define IPC_DEBUG_LOG(...) do { if (kDebugIpcMessages) NSLog(__VA_ARGS__); } while(0)

static std::string GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    time_t time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

static std::string HexEncode(const std::string& input) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char c : input) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0x0f]);
    }
    return out;
}

static std::string HexDecode(const std::string& hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

@implementation TBIpcClient {
    std::unique_ptr<ipc::UnixSocketConnection> _connection;
    std::mutex _sendLock;
    std::mutex _disconnectLock;
    std::atomic<bool> _running;
    std::thread _recvThread;
    // Console batching: accumulate text, flush on a coalesced timer
    std::mutex _consoleLock;
    NSMutableString* _consoleBatch;
    std::atomic<bool> _consoleFlushScheduled;
    // Shared-memory framebuffer for zero-copy frame transport
    ipc::SharedFramebuffer _shmFb;
}

- (BOOL)connectToVm:(NSString *)vmId {
    std::string path = ipc::GetSocketPath(vmId.UTF8String);
    auto conn = ipc::UnixSocketClient::Connect(path);
    if (!conn.IsValid()) return NO;

    _connection = std::make_unique<ipc::UnixSocketConnection>(std::move(conn));
    return YES;
}

- (BOOL)attachToFd:(int)fd {
    if (fd < 0) return NO;
    _connection = std::make_unique<ipc::UnixSocketConnection>(fd);
    return YES;
}

- (void)disconnect {
    std::lock_guard<std::mutex> lock(_disconnectLock);
    _running = false;
    if (_connection) {
        _connection->Close();
    }
    if (_recvThread.joinable()) {
        if (_recvThread.get_id() == std::this_thread::get_id()) {
            _recvThread.detach();
        } else {
            _recvThread.join();
        }
    }
    _connection.reset();
    _shmFb.Close();
}

- (BOOL)isConnected {
    return _connection && _connection->IsValid();
}

#pragma mark - Send: Control

- (BOOL)sendControlCommand:(NSString *)command {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kControl;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "runtime.command";
    msg.fields["command"] = command.UTF8String;

    std::lock_guard<std::mutex> lock(_sendLock);
    std::string encoded = ipc::Encode(msg);
    IPC_DEBUG_LOG(@"[IPC] >> %s [%zu bytes] command=%s", GetTimestamp().c_str(), encoded.size(), command.UTF8String);
    return _connection->Send(encoded);
}

#pragma mark - Send: Input

- (BOOL)sendKeyEvent:(uint16_t)code pressed:(BOOL)pressed {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.key_event";
    msg.fields["key_code"] = std::to_string(code);
    msg.fields["pressed"] = pressed ? "1" : "0";

    std::lock_guard<std::mutex> lock(_sendLock);
    std::string encoded = ipc::Encode(msg);
    IPC_DEBUG_LOG(@"[IPC] >> %s [%zu bytes] key_code=%u pressed=%d", GetTimestamp().c_str(), encoded.size(), code, pressed);
    return _connection->Send(encoded);
}

- (BOOL)sendPointerAbsolute:(int32_t)x y:(int32_t)y buttons:(uint32_t)buttons {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.pointer_event";
    msg.fields["x"] = std::to_string(x);
    msg.fields["y"] = std::to_string(y);
    msg.fields["buttons"] = std::to_string(buttons);

    std::lock_guard<std::mutex> lock(_sendLock);
    std::string encoded = ipc::Encode(msg);
    IPC_DEBUG_LOG(@"[IPC] >> %s [%zu bytes] pointer x=%d y=%d buttons=%u", GetTimestamp().c_str(), encoded.size(), x, y, buttons);
    return _connection->Send(encoded);
}

- (BOOL)sendScrollEvent:(int32_t)delta {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.wheel_event";
    msg.fields["delta"] = std::to_string(delta);

    std::lock_guard<std::mutex> lock(_sendLock);
    std::string encoded = ipc::Encode(msg);
    IPC_DEBUG_LOG(@"[IPC] >> %s [%zu bytes] wheel delta=%d", GetTimestamp().c_str(), encoded.size(), delta);
    return _connection->Send(encoded);
}

#pragma mark - Send: Console

- (BOOL)sendConsoleInput:(NSString *)text {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kConsole;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "console.input";
    std::string raw = text.UTF8String;
    msg.fields["data_hex"] = HexEncode(raw);

    std::lock_guard<std::mutex> lock(_sendLock);
    std::string encoded = ipc::Encode(msg);
    IPC_DEBUG_LOG(@"[IPC] >> %s [%zu bytes] console_input len=%zu", GetTimestamp().c_str(), encoded.size(), raw.size());
    return _connection->Send(encoded);
}

#pragma mark - Send: VM Configuration Updates

- (BOOL)sendSharedFoldersUpdate:(NSArray<NSString *> *)entries {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kControl;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "runtime.update_shared_folders";
    msg.fields["folder_count"] = std::to_string(entries.count);
    for (NSUInteger i = 0; i < entries.count; ++i) {
        msg.fields["folder_" + std::to_string(i)] = entries[i].UTF8String;
    }

    std::lock_guard<std::mutex> lock(_sendLock);
    return _connection->Send(ipc::Encode(msg));
}

- (BOOL)sendPortForwardsUpdate:(NSArray<NSString *> *)entries netEnabled:(BOOL)netEnabled {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kControl;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "runtime.update_network";
    msg.fields["link_up"] = netEnabled ? "true" : "false";
    msg.fields["forward_count"] = std::to_string(entries.count);
    for (NSUInteger i = 0; i < entries.count; ++i) {
        msg.fields["forward_" + std::to_string(i)] = entries[i].UTF8String;
    }

    std::lock_guard<std::mutex> lock(_sendLock);
    return _connection->Send(ipc::Encode(msg));
}

#pragma mark - Send: Display

- (BOOL)sendDisplaySetSizeWidth:(uint32_t)width height:(uint32_t)height {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kDisplay;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "display.set_size";
    msg.fields["width"] = std::to_string(width);
    msg.fields["height"] = std::to_string(height);

    std::lock_guard<std::mutex> lock(_sendLock);
    return _connection->Send(ipc::Encode(msg));
}

#pragma mark - Send: Clipboard

- (BOOL)sendClipboardGrab:(NSArray<NSNumber *> *)types {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.grab";
    std::string typesStr;
    for (NSUInteger i = 0; i < types.count; ++i) {
        if (i > 0) typesStr += ",";
        typesStr += std::to_string(types[i].unsignedIntValue);
    }
    msg.fields["types"] = typesStr;

    std::lock_guard<std::mutex> lock(_sendLock);
    std::string encoded = ipc::Encode(msg);
    IPC_DEBUG_LOG(@"[IPC] >> %s [%zu bytes] clipboard.grab types=%s", GetTimestamp().c_str(), encoded.size(), typesStr.c_str());
    return _connection->Send(encoded);
}

- (BOOL)sendClipboardData:(uint32_t)dataType payload:(NSData *)payload {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.data";
    msg.fields["data_type"] = std::to_string(dataType);
    if (payload.length > 0) {
        msg.payload.assign(
            static_cast<const uint8_t*>(payload.bytes),
            static_cast<const uint8_t*>(payload.bytes) + payload.length
        );
    }

    std::lock_guard<std::mutex> lock(_sendLock);
    std::string encoded = ipc::Encode(msg);
    IPC_DEBUG_LOG(@"[IPC] >> %s [%zu bytes] clipboard.data type=%u payload=%zu", GetTimestamp().c_str(), encoded.size(), dataType, payload.length);
    return _connection->Send(encoded);
}

- (BOOL)sendClipboardRequest:(uint32_t)dataType {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.request";
    msg.fields["data_type"] = std::to_string(dataType);

    std::lock_guard<std::mutex> lock(_sendLock);
    std::string encoded = ipc::Encode(msg);
    IPC_DEBUG_LOG(@"[IPC] >> %s [%zu bytes] clipboard.request type=%u", GetTimestamp().c_str(), encoded.size(), dataType);
    return _connection->Send(encoded);
}

- (BOOL)sendClipboardRelease {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.release";

    std::lock_guard<std::mutex> lock(_sendLock);
    std::string encoded = ipc::Encode(msg);
    IPC_DEBUG_LOG(@"[IPC] >> %s [%zu bytes] clipboard.release", GetTimestamp().c_str(), encoded.size());
    return _connection->Send(encoded);
}

#pragma mark - Receive Loop

- (void)startReceiveLoopWithFrameHandler:(void (^)(NSData *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))frameHandler
                           cursorHandler:(void (^)(BOOL, BOOL, uint32_t, uint32_t, uint32_t, uint32_t, NSData * _Nullable))cursorHandler
                            audioHandler:(void (^)(NSData *, uint32_t, uint16_t))audioHandler
                         consoleHandler:(void (^)(NSString *))consoleHandler
                    clipboardGrabHandler:(void (^)(NSArray<NSNumber *> *))clipboardGrabHandler
                    clipboardDataHandler:(void (^)(uint32_t, NSData *))clipboardDataHandler
                 clipboardRequestHandler:(void (^)(uint32_t))clipboardRequestHandler
                    runtimeStateHandler:(void (^)(NSString *))runtimeStateHandler
                  guestAgentStateHandler:(void (^)(BOOL))guestAgentStateHandler
                    displayStateHandler:(void (^)(BOOL, uint32_t, uint32_t))displayStateHandler
                       disconnectHandler:(void (^)(void))disconnectHandler {
    if (_recvThread.joinable()) {
        _running = false;
        if (_connection) _connection->Close();
        _recvThread.join();
    }

    _running = true;

    typedef void (^FrameBlock)(NSData *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    typedef void (^CursorBlock)(BOOL, BOOL, uint32_t, uint32_t, uint32_t, uint32_t, NSData * _Nullable);
    typedef void (^AudioBlock)(NSData *, uint32_t, uint16_t);
    typedef void (^ConsoleBlock)(NSString *);
    typedef void (^ClipGrabBlock)(NSArray<NSNumber *> *);
    typedef void (^ClipDataBlock)(uint32_t, NSData *);
    typedef void (^ClipReqBlock)(uint32_t);
    typedef void (^StateBlock)(NSString *);
    typedef void (^BoolBlock)(BOOL);
    typedef void (^DispStateBlock)(BOOL, uint32_t, uint32_t);
    typedef void (^VoidBlock)(void);

    FrameBlock     fh  = [frameHandler copy];
    CursorBlock    cuH = [cursorHandler copy];
    AudioBlock     ah  = [audioHandler copy];
    ConsoleBlock   coh = [consoleHandler copy];
    ClipGrabBlock  cgH = [clipboardGrabHandler copy];
    ClipDataBlock  cdH = [clipboardDataHandler copy];
    ClipReqBlock   crH = [clipboardRequestHandler copy];
    StateBlock     rsH = [runtimeStateHandler copy];
    BoolBlock      gaH = [guestAgentStateHandler copy];
    DispStateBlock dsH = [displayStateHandler copy];
    VoidBlock      dh  = [disconnectHandler copy];

    _recvThread = std::thread([self, fh, cuH, ah, coh, cgH, cdH, crH, rsH, gaH, dsH, dh] {
        // Streaming parser — mirrors the Windows DispatchPipeData approach.
        // One large read buffer, parse header lines + payloads in-place.
        std::string pending;
        pending.reserve(8 * 1024 * 1024);
        char readbuf[256 * 1024];

        size_t payload_needed = 0;
        ipc::Message pending_msg;

        auto dispatchMessages = [&]() {
            while (!pending.empty()) {
                if (payload_needed > 0) {
                    if (pending.size() < payload_needed) return;
                    pending_msg.payload.assign(
                        reinterpret_cast<const uint8_t*>(pending.data()),
                        reinterpret_cast<const uint8_t*>(pending.data()) + payload_needed);
                    pending.erase(0, payload_needed);
                    payload_needed = 0;

                    auto& msg = pending_msg;
                    [self dispatchMsg:msg fh:fh cuH:cuH ah:ah coh:coh cgH:cgH cdH:cdH crH:crH rsH:rsH gaH:gaH dsH:dsH];
                    continue;
                }

                size_t nl = pending.find('\n');
                if (nl == std::string::npos) return;
                std::string line = pending.substr(0, nl + 1);
                pending.erase(0, nl + 1);

                auto decoded = ipc::Decode(line);
                if (!decoded) continue;

                auto ps_it = decoded->fields.find("payload_size");
                if (ps_it != decoded->fields.end()) {
                    payload_needed = std::strtoull(ps_it->second.c_str(), nullptr, 10);
                    decoded->fields.erase(ps_it);
                    if (payload_needed > 0) {
                        pending_msg = std::move(*decoded);
                        continue;
                    }
                }

                auto& msg = *decoded;
                [self dispatchMsg:msg fh:fh cuH:cuH ah:ah coh:coh cgH:cgH cdH:cdH crH:crH rsH:rsH gaH:gaH dsH:dsH];
            }
        };

        int fd = self->_connection->fd();
        while (self->_running && self->_connection && self->_connection->IsValid()) {
            ssize_t n = ::read(fd, readbuf, sizeof(readbuf));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            IPC_DEBUG_LOG(@"[IPC] << %s [%zd bytes] received", GetTimestamp().c_str(), n);
            pending.append(readbuf, static_cast<size_t>(n));
            dispatchMessages();
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            dh();
        });
    });
}

- (void)dispatchMsg:(ipc::Message&)msg
                 fh:(void (^)(NSData *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))fh
                cuH:(void (^)(BOOL, BOOL, uint32_t, uint32_t, uint32_t, uint32_t, NSData * _Nullable))cuH
                 ah:(void (^)(NSData *, uint32_t, uint16_t))ah
                coh:(void (^)(NSString *))coh
                cgH:(void (^)(NSArray<NSNumber *> *))cgH
                cdH:(void (^)(uint32_t, NSData *))cdH
                crH:(void (^)(uint32_t))crH
                rsH:(void (^)(NSString *))rsH
                gaH:(void (^)(BOOL))gaH
                dsH:(void (^)(BOOL, uint32_t, uint32_t))dsH {

    auto getU32 = [&](const char* key) -> uint32_t {
        auto fi = msg.fields.find(key);
        return (fi != msg.fields.end()) ? static_cast<uint32_t>(std::strtoul(fi->second.c_str(), nullptr, 10)) : 0;
    };

    if (msg.type == "display.shm_init") {
        auto ni = msg.fields.find("shm_name");
        if (ni == msg.fields.end()) return;
        uint32_t w = getU32("width");
        uint32_t h = getU32("height");
        if (w == 0 || h == 0) return;
        IPC_DEBUG_LOG(@"[IPC] << %s display.shm_init %ux%u", GetTimestamp().c_str(), w, h);
        _shmFb.Close();
        if (!_shmFb.Open(ni->second, w, h)) {
            IPC_DEBUG_LOG(@"[IPC] failed to open shared framebuffer: %s %ux%u",
                  ni->second.c_str(), w, h);
        }
    }
    else if (msg.type == "display.frame_ready") {
        uint32_t w = getU32("width");
        uint32_t h = getU32("height");
        uint32_t stride = getU32("stride");
        uint32_t resW = getU32("resource_width");
        uint32_t resH = getU32("resource_height");
        uint32_t dirtyX = getU32("dirty_x");
        uint32_t dirtyY = getU32("dirty_y");
        if (resW == 0) resW = w;
        if (resH == 0) resH = h;
        if (w == 0 || h == 0 || stride == 0) return;
        if (!_shmFb.IsValid() || _shmFb.width() != resW || _shmFb.height() != resH) return;

        IPC_DEBUG_LOG(@"[IPC] << %s display.frame_ready %ux%u [%u,%u]", GetTimestamp().c_str(), w, h, dirtyX, dirtyY);

        // Zero-copy: wrap SHM pointer directly into NSData.
        // Point to the first row of the dirty rect; pass the full SHM stride
        // so Metal's texture.replace can stride-skip between rows.
        uint32_t shm_stride = _shmFb.stride();
        size_t offset = static_cast<size_t>(dirtyY) * shm_stride +
                        static_cast<size_t>(dirtyX) * 4;
        size_t needed = static_cast<size_t>(h - 1) * shm_stride + static_cast<size_t>(w) * 4;
        if (offset + needed > _shmFb.size()) return;

        NSData* pixels = [NSData dataWithBytesNoCopy:_shmFb.data() + offset
                                              length:needed
                                        freeWhenDone:NO];
        fh(pixels, w, h, shm_stride, resW, resH, dirtyX, dirtyY);
    }
    else if (msg.type == "display.frame") {
        // Legacy fallback path: frame data sent via IPC payload.
        uint32_t w = getU32("width");
        uint32_t h = getU32("height");
        uint32_t stride = getU32("stride");
        uint32_t resW = getU32("resource_width");
        uint32_t resH = getU32("resource_height");
        uint32_t dirtyX = getU32("dirty_x");
        uint32_t dirtyY = getU32("dirty_y");
        if (resW == 0) resW = w;
        if (resH == 0) resH = h;
        if (w == 0 || h == 0 || stride == 0 || msg.payload.empty()) return;

        IPC_DEBUG_LOG(@"[IPC] << %s display.frame %ux%u [%zu bytes payload]", GetTimestamp().c_str(), w, h, msg.payload.size());

        NSData* pixels = [NSData dataWithBytesNoCopy:(void*)msg.payload.data()
                                              length:msg.payload.size()
                                        freeWhenDone:NO];
        fh(pixels, w, h, stride, resW, resH, dirtyX, dirtyY);
    }
    else if (msg.type == "display.cursor") {
        BOOL visible = (getU32("visible") != 0);
        BOOL imageUpdated = (getU32("image_updated") != 0);
        uint32_t w = getU32("width");
        uint32_t h = getU32("height");
        uint32_t hotX = getU32("hot_x");
        uint32_t hotY = getU32("hot_y");
        IPC_DEBUG_LOG(@"[IPC] << %s display.cursor visible=%d updated=%d %ux%u", GetTimestamp().c_str(), visible, imageUpdated, w, h);
        NSData* pixels = nil;
        if (imageUpdated && !msg.payload.empty()) {
            pixels = [NSData dataWithBytes:msg.payload.data()
                                    length:msg.payload.size()];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            cuH(visible, imageUpdated, w, h, hotX, hotY, pixels);
        });
    }
    else if (msg.type == "audio.pcm") {
        uint32_t rate = 48000;
        uint16_t channels = 2;
        auto ri = msg.fields.find("sample_rate");
        auto ci = msg.fields.find("channels");
        if (ri != msg.fields.end()) rate = std::stoul(ri->second);
        if (ci != msg.fields.end()) channels = static_cast<uint16_t>(std::stoul(ci->second));
        IPC_DEBUG_LOG(@"[IPC] << %s audio.pcm [%zu bytes] rate=%u ch=%u", GetTimestamp().c_str(), msg.payload.size(), rate, channels);
        NSData* pcm = [NSData dataWithBytes:msg.payload.data()
                                     length:msg.payload.size()];
        ah(pcm, rate, channels);
    }
    else if (msg.type == "console.data") {
        auto di = msg.fields.find("data_hex");
        if (di != msg.fields.end()) {
            std::string raw = HexDecode(di->second);
            IPC_DEBUG_LOG(@"[IPC] << %s console.data [%zu bytes]", GetTimestamp().c_str(), raw.size());
            NSString* text = [[NSString alloc] initWithBytes:raw.data()
                                                      length:raw.size()
                                                    encoding:NSUTF8StringEncoding];
            if (!text) {
                text = [[NSString alloc] initWithBytes:raw.data()
                                                length:raw.size()
                                              encoding:NSISOLatin1StringEncoding];
            }
            if (text) {
                {
                    std::lock_guard<std::mutex> cl(_consoleLock);
                    if (!_consoleBatch) _consoleBatch = [NSMutableString new];
                    [_consoleBatch appendString:text];
                }
                if (!_consoleFlushScheduled.exchange(true)) {
                    dispatch_after(
                        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(50 * NSEC_PER_MSEC)),
                        dispatch_get_main_queue(), ^{
                            NSString* batch;
                            {
                                std::lock_guard<std::mutex> cl(self->_consoleLock);
                                batch = [self->_consoleBatch copy];
                                [self->_consoleBatch setString:@""];
                            }
                            self->_consoleFlushScheduled = false;
                            if (batch.length > 0) coh(batch);
                        });
                }
            }
        }
    }
    else if (msg.type == "clipboard.grab") {
        auto ti = msg.fields.find("types");
        if (ti != msg.fields.end()) {
            IPC_DEBUG_LOG(@"[IPC] << %s clipboard.grab types=%s", GetTimestamp().c_str(), ti->second.c_str());
            NSMutableArray<NSNumber *>* types = [NSMutableArray array];
            std::string typesStr = ti->second;
            size_t pos = 0;
            while (pos < typesStr.size()) {
                size_t comma = typesStr.find(',', pos);
                if (comma == std::string::npos) comma = typesStr.size();
                std::string numStr = typesStr.substr(pos, comma - pos);
                if (!numStr.empty()) {
                    [types addObject:@(static_cast<uint32_t>(std::strtoul(numStr.c_str(), nullptr, 10)))];
                }
                pos = comma + 1;
            }
            dispatch_async(dispatch_get_main_queue(), ^{ cgH(types); });
        }
    }
    else if (msg.type == "clipboard.data") {
        uint32_t dataType = 0;
        auto dti = msg.fields.find("data_type");
        if (dti != msg.fields.end())
            dataType = static_cast<uint32_t>(std::strtoul(dti->second.c_str(), nullptr, 10));
        IPC_DEBUG_LOG(@"[IPC] << %s clipboard.data type=%u [%zu bytes]", GetTimestamp().c_str(), dataType, msg.payload.size());
        NSData* payload = [NSData dataWithBytes:msg.payload.data() length:msg.payload.size()];
        dispatch_async(dispatch_get_main_queue(), ^{ cdH(dataType, payload); });
    }
    else if (msg.type == "clipboard.request") {
        uint32_t dataType = 0;
        auto dti = msg.fields.find("data_type");
        if (dti != msg.fields.end())
            dataType = static_cast<uint32_t>(std::strtoul(dti->second.c_str(), nullptr, 10));
        IPC_DEBUG_LOG(@"[IPC] << %s clipboard.request type=%u", GetTimestamp().c_str(), dataType);
        dispatch_async(dispatch_get_main_queue(), ^{ crH(dataType); });
    }
    else if (msg.type == "runtime.state") {
        auto si = msg.fields.find("state");
        if (si != msg.fields.end()) {
            IPC_DEBUG_LOG(@"[IPC] << %s runtime.state %s", GetTimestamp().c_str(), si->second.c_str());
            NSString* state = [NSString stringWithUTF8String:si->second.c_str()];
            dispatch_async(dispatch_get_main_queue(), ^{ rsH(state); });
        }
    }
    else if (msg.type == "guest_agent.state") {
        auto ci = msg.fields.find("connected");
        BOOL connected = (ci != msg.fields.end() && ci->second == "1");
        IPC_DEBUG_LOG(@"[IPC] << %s guest_agent.state connected=%d", GetTimestamp().c_str(), connected);
        dispatch_async(dispatch_get_main_queue(), ^{ gaH(connected); });
    }
    else if (msg.type == "display.state") {
        auto ai = msg.fields.find("active");
        auto wi = msg.fields.find("width");
        auto hi = msg.fields.find("height");
        BOOL active = (ai != msg.fields.end() && ai->second == "1");
        uint32_t w = (wi != msg.fields.end()) ? std::stoul(wi->second) : 0;
        uint32_t h = (hi != msg.fields.end()) ? std::stoul(hi->second) : 0;
        IPC_DEBUG_LOG(@"[IPC] << %s display.state active=%d %ux%u", GetTimestamp().c_str(), active, w, h);
        dispatch_async(dispatch_get_main_queue(), ^{ dsH(active, w, h); });
    }
}

- (void)stopReceiveLoop {
    _running = false;
    if (_connection) {
        _connection->Close();
    }
    if (_recvThread.joinable()) {
        if (_recvThread.get_id() == std::this_thread::get_id()) {
            _recvThread.detach();
        } else {
            _recvThread.join();
        }
    }
}

- (void)dealloc {
    [self disconnect];
}

@end
