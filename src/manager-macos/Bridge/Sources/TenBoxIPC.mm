#import "TenBoxIPC.h"
#include "ipc/unix_socket.h"
#include "ipc/protocol_v1.h"
#include <string>
#include <mutex>
#include <atomic>
#include <thread>

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
    std::atomic<bool> _running;
    std::thread _recvThread;
    // Console batching: accumulate text, flush on a coalesced timer
    std::mutex _consoleLock;
    NSMutableString* _consoleBatch;
    std::atomic<bool> _consoleFlushScheduled;
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
    _running = false;
    if (_connection) {
        _connection->Close();
    }
    if (_recvThread.joinable()) {
        _recvThread.join();
    }
    _connection.reset();
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
    return _connection->Send(ipc::Encode(msg));
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
    return _connection->Send(ipc::Encode(msg));
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
    return _connection->Send(ipc::Encode(msg));
}

- (BOOL)sendScrollEvent:(int32_t)delta {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.wheel_event";
    msg.fields["delta"] = std::to_string(delta);

    std::lock_guard<std::mutex> lock(_sendLock);
    return _connection->Send(ipc::Encode(msg));
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
    msg.kind = ipc::Kind::kEvent;
    msg.type = "clipboard.grab";
    std::string typesStr;
    for (NSUInteger i = 0; i < types.count; ++i) {
        if (i > 0) typesStr += ",";
        typesStr += std::to_string(types[i].unsignedIntValue);
    }
    msg.fields["types"] = typesStr;

    std::lock_guard<std::mutex> lock(_sendLock);
    return _connection->Send(ipc::Encode(msg));
}

- (BOOL)sendClipboardData:(uint32_t)dataType payload:(NSData *)payload {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kEvent;
    msg.type = "clipboard.data";
    msg.fields["data_type"] = std::to_string(dataType);
    if (payload.length > 0) {
        msg.payload.assign(
            static_cast<const uint8_t*>(payload.bytes),
            static_cast<const uint8_t*>(payload.bytes) + payload.length
        );
    }

    std::lock_guard<std::mutex> lock(_sendLock);
    return _connection->Send(ipc::Encode(msg));
}

- (BOOL)sendClipboardRequest:(uint32_t)dataType {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kEvent;
    msg.type = "clipboard.request";
    msg.fields["data_type"] = std::to_string(dataType);

    std::lock_guard<std::mutex> lock(_sendLock);
    return _connection->Send(ipc::Encode(msg));
}

- (BOOL)sendClipboardRelease {
    if (!_connection || !_connection->IsValid()) return NO;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kEvent;
    msg.type = "clipboard.release";

    std::lock_guard<std::mutex> lock(_sendLock);
    return _connection->Send(ipc::Encode(msg));
}

#pragma mark - Receive Loop

- (void)startReceiveLoopWithFrameHandler:(void (^)(NSData *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))frameHandler
                            audioHandler:(void (^)(NSData *, uint32_t, uint16_t))audioHandler
                         consoleHandler:(void (^)(NSString *))consoleHandler
                    clipboardGrabHandler:(void (^)(NSArray<NSNumber *> *))clipboardGrabHandler
                    clipboardDataHandler:(void (^)(uint32_t, NSData *))clipboardDataHandler
                 clipboardRequestHandler:(void (^)(uint32_t))clipboardRequestHandler
                    runtimeStateHandler:(void (^)(NSString *))runtimeStateHandler
                  guestAgentStateHandler:(void (^)(BOOL))guestAgentStateHandler
                    displayStateHandler:(void (^)(BOOL, uint32_t, uint32_t))displayStateHandler
                       disconnectHandler:(void (^)(void))disconnectHandler {
    _running = true;

    typedef void (^FrameBlock)(NSData *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
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
    AudioBlock     ah  = [audioHandler copy];
    ConsoleBlock   coh = [consoleHandler copy];
    ClipGrabBlock  cgH = [clipboardGrabHandler copy];
    ClipDataBlock  cdH = [clipboardDataHandler copy];
    ClipReqBlock   crH = [clipboardRequestHandler copy];
    StateBlock     rsH = [runtimeStateHandler copy];
    BoolBlock      gaH = [guestAgentStateHandler copy];
    DispStateBlock dsH = [displayStateHandler copy];
    VoidBlock      dh  = [disconnectHandler copy];

    _recvThread = std::thread([self, fh, ah, coh, cgH, cdH, crH, rsH, gaH, dsH, dh] {
        while (self->_running && self->_connection && self->_connection->IsValid()) {
            std::string line = self->_connection->ReadLine();
            if (line.empty()) {
                break;
            }

            auto decoded = ipc::Decode(line);
            if (!decoded) continue;

            auto& msg = *decoded;

            auto it = msg.fields.find("payload_size");
            if (it != msg.fields.end()) {
                size_t psize = std::stoull(it->second);
                msg.payload.resize(psize);
                if (!self->_connection->ReadExact(msg.payload.data(), psize)) {
                    break;
                }
            }

            // Display frame (dirty rect update within a resource)
            if (msg.type == "display.frame") {
                auto getU32 = [&](const char* key) -> uint32_t {
                    auto fi = msg.fields.find(key);
                    return (fi != msg.fields.end()) ? static_cast<uint32_t>(std::stoul(fi->second)) : 0;
                };
                uint32_t w = getU32("width");
                uint32_t h = getU32("height");
                uint32_t stride = getU32("stride");
                uint32_t resW = getU32("resource_width");
                uint32_t resH = getU32("resource_height");
                uint32_t dirtyX = getU32("dirty_x");
                uint32_t dirtyY = getU32("dirty_y");

                if (resW == 0) resW = w;
                if (resH == 0) resH = h;

                if (w == 0 || h == 0 || stride == 0 || msg.payload.empty()) {
                    continue;
                }

                NSData* pixels = [NSData dataWithBytes:msg.payload.data()
                                                length:msg.payload.size()];
                fh(pixels, w, h, stride, resW, resH, dirtyX, dirtyY);
            }
            // Audio PCM
            else if (msg.type == "audio.pcm") {
                uint32_t rate = 44100;
                uint16_t channels = 2;
                auto ri = msg.fields.find("sample_rate");
                auto ci = msg.fields.find("channels");
                if (ri != msg.fields.end()) rate = std::stoul(ri->second);
                if (ci != msg.fields.end()) channels = static_cast<uint16_t>(std::stoul(ci->second));

                NSData* pcm = [NSData dataWithBytes:msg.payload.data()
                                             length:msg.payload.size()];
                dispatch_async(dispatch_get_main_queue(), ^{
                    ah(pcm, rate, channels);
                });
            }
            // Console data — batch and flush to avoid flooding the main thread
            else if (msg.type == "console.data") {
                auto di = msg.fields.find("data_hex");
                if (di != msg.fields.end()) {
                    std::string raw = HexDecode(di->second);
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
                            std::lock_guard<std::mutex> cl(self->_consoleLock);
                            if (!self->_consoleBatch) {
                                self->_consoleBatch = [NSMutableString new];
                            }
                            [self->_consoleBatch appendString:text];
                        }
                        if (!self->_consoleFlushScheduled.exchange(true)) {
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
                                    if (batch.length > 0) {
                                        coh(batch);
                                    }
                                });
                        }
                    }
                }
            }
            // Clipboard grab (from guest)
            else if (msg.type == "clipboard.grab") {
                auto ti = msg.fields.find("types");
                if (ti != msg.fields.end()) {
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
                    dispatch_async(dispatch_get_main_queue(), ^{
                        cgH(types);
                    });
                }
            }
            // Clipboard data (from guest)
            else if (msg.type == "clipboard.data") {
                uint32_t dataType = 0;
                auto dti = msg.fields.find("data_type");
                if (dti != msg.fields.end()) {
                    dataType = static_cast<uint32_t>(std::strtoul(dti->second.c_str(), nullptr, 10));
                }
                NSData* payload = [NSData dataWithBytes:msg.payload.data()
                                                 length:msg.payload.size()];
                dispatch_async(dispatch_get_main_queue(), ^{
                    cdH(dataType, payload);
                });
            }
            // Clipboard request (from guest)
            else if (msg.type == "clipboard.request") {
                uint32_t dataType = 0;
                auto dti = msg.fields.find("data_type");
                if (dti != msg.fields.end()) {
                    dataType = static_cast<uint32_t>(std::strtoul(dti->second.c_str(), nullptr, 10));
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    crH(dataType);
                });
            }
            // Runtime state
            else if (msg.type == "runtime.state") {
                auto si = msg.fields.find("state");
                if (si != msg.fields.end()) {
                    NSString* state = [NSString stringWithUTF8String:si->second.c_str()];
                    dispatch_async(dispatch_get_main_queue(), ^{
                        rsH(state);
                    });
                }
            }
            // Guest agent state
            else if (msg.type == "guest_agent.state") {
                auto ci = msg.fields.find("connected");
                BOOL connected = NO;
                if (ci != msg.fields.end()) {
                    connected = (ci->second == "1");
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    gaH(connected);
                });
            }
            // Display state
            else if (msg.type == "display.state") {
                auto ai = msg.fields.find("active");
                auto wi = msg.fields.find("width");
                auto hi = msg.fields.find("height");
                BOOL active = NO;
                uint32_t w = 0, h = 0;
                if (ai != msg.fields.end()) active = (ai->second == "1");
                if (wi != msg.fields.end()) w = std::stoul(wi->second);
                if (hi != msg.fields.end()) h = std::stoul(hi->second);
                dispatch_async(dispatch_get_main_queue(), ^{
                    dsH(active, w, h);
                });
            }
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            dh();
        });
    });
}

- (void)stopReceiveLoop {
    _running = false;
    if (_connection) {
        _connection->Close();
    }
    if (_recvThread.joinable()) {
        _recvThread.join();
    }
}

- (void)dealloc {
    [self disconnect];
}

@end
