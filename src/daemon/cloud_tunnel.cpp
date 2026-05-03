#include "daemon/cloud_tunnel.h"

#include "daemon/host_updater.h"
#include "daemon/kvm_doctor.h"
#include "daemon/media_interfaces.h"
#include "daemon/remote_webrtc.h"
#include "daemon/resource_monitor.h"
#include "common/image_source.h"
#include "version.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string_view>
#include <thread>

namespace tenbox::daemon {
namespace {

struct WsUrl {
    std::string host;
    std::string port = "80";
    std::string target = "/";
    std::string host_id;
    bool tls = false;
};

nlohmann::json Ok(nlohmann::json payload = nlohmann::json::object()) {
    return {{"ok", true}, {"payload", std::move(payload)}};
}

nlohmann::json Error(std::string code, std::string message) {
    return {{"ok", false}, {"error_code", std::move(code)}, {"error", std::move(message)}};
}

uint32_t AlignDisplaySize(uint32_t value) {
    value = std::max<uint32_t>(320, std::min<uint32_t>(3840, value));
    return (value + 4u) & ~7u;
}

uint32_t ClampVideoBitrate(uint32_t value) {
    if (value == 0) return 4'000'000;
    return std::max<uint32_t>(500'000, std::min<uint32_t>(20'000'000, value));
}

PixelFormat ParseRemoteVideoPixelFormat(const nlohmann::json& payload) {
    const std::string value = payload.value("video_pixel_format", "yuv420p");
    if (value == "yuv444p") return PixelFormat::kYuv444p;
    return PixelFormat::kYuv420p;
}

const char* RemoteVideoPixelFormatName(PixelFormat format) {
    return format == PixelFormat::kYuv444p ? "yuv444p" : "yuv420p";
}

// vdagent type code values (see core/vdagent/vdagent_protocol.h). Duplicated
// here so cloud_tunnel can speak browser-friendly MIME types without taking a
// build dependency on the vmm headers.
constexpr uint32_t kVdAgentClipboardUtf8Text = 1;
constexpr uint32_t kVdAgentClipboardImagePng = 2;

uint32_t MimeToVdAgentType(const std::string& mime) {
    if (mime == "text/plain") return kVdAgentClipboardUtf8Text;
    if (mime == "image/png") return kVdAgentClipboardImagePng;
    return 0;
}

const char* VdAgentTypeToMime(uint32_t value) {
    switch (value) {
    case kVdAgentClipboardUtf8Text: return "text/plain";
    case kVdAgentClipboardImagePng: return "image/png";
    default: return nullptr;
    }
}

std::string Base64Encode(const std::vector<uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t b0 = bytes[i];
        const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const uint32_t value = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(value >> 18) & 0x3f]);
        out.push_back(kAlphabet[(value >> 12) & 0x3f]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(value >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[value & 0x3f] : '=');
    }
    return out;
}

std::vector<uint8_t> Base64Decode(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
        if (c >= '0' && c <= '9') return 52 + (c - '0');
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve((text.size() / 4) * 3);
    int buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = value(c);
        if (v < 0) continue;
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

bool ParseWsUrl(const std::string& url, WsUrl* out, std::string* error) {
    // Phase 3 accepts both ws:// (dev / loopback) and wss:// (production via
    // LB at my.tenbox.ai). Default port flips with the scheme.
    constexpr const char* kPlain = "ws://";
    constexpr const char* kSecure = "wss://";
    std::string rest;
    if (url.rfind(kSecure, 0) == 0) {
        out->tls = true;
        out->port = "443";
        rest = url.substr(std::strlen(kSecure));
    } else if (url.rfind(kPlain, 0) == 0) {
        out->tls = false;
        out->port = "80";
        rest = url.substr(std::strlen(kPlain));
    } else {
        if (error) *error = "cloud URL must start with ws:// or wss://";
        return false;
    }
    const auto slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    out->target = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos) {
        out->host = authority;
    } else {
        out->host = authority.substr(0, colon);
        out->port = authority.substr(colon + 1);
    }
    const std::string marker = "host_id=";
    const auto host_id_pos = out->target.find(marker);
    if (host_id_pos != std::string::npos) {
        out->host_id = out->target.substr(host_id_pos + marker.size());
        const auto amp = out->host_id.find('&');
        if (amp != std::string::npos) out->host_id.resize(amp);
    }
    if (out->host.empty()) {
        if (error) *error = "cloud URL host is empty";
        return false;
    }
    return true;
}

std::string Hostname() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) return buf;
    return "tenbox-host";
}

std::string LoadOrCreateDeviceId(const std::string& data_dir) {
    const auto path = std::filesystem::path(data_dir) / "device.id";
    std::ifstream in(path);
    std::string id;
    if (in >> id; !id.empty()) return id;

    id = GenerateUuid();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << id << "\n";
    return id;
}

// device.token holds the long-lived shared secret minted by the cloud after
// the operator confirms pairing in the browser. Phase 3 stores it raw on
// disk at 0600; Phase 6 will move to a signed challenge/response.
std::filesystem::path DeviceTokenPath(const std::string& data_dir) {
    return std::filesystem::path(data_dir) / "device.token";
}

std::string LoadDeviceToken(const std::string& data_dir) {
    std::ifstream in(DeviceTokenPath(data_dir));
    std::string token;
    in >> token;
    return token;
}

bool DeleteDeviceToken(const std::string& data_dir, std::string* error) {
    const auto path = DeviceTokenPath(data_dir);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        if (error) *error = "failed to remove " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool SaveDeviceToken(const std::string& data_dir, const std::string& token, std::string* error) {
    const auto path = DeviceTokenPath(data_dir);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "failed to mkdir " + path.parent_path().string() + ": " + ec.message();
        return false;
    }
    const auto tmp = path.string() + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        if (error) *error = std::string("open ") + tmp + " failed: " + std::strerror(errno);
        return false;
    }
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(token.size())) {
        const ssize_t n = ::write(fd, token.data() + written,
                                  token.size() - static_cast<size_t>(written));
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            if (error) *error = std::string("write failed: ") + std::strerror(errno);
            return false;
        }
        written += n;
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        if (error) *error = std::string("rename failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

// 8-digit decimal pair code, sourced from /dev/urandom so the operator can
// trust this against an offline guess; falls back to std::random_device if
// urandom is unavailable. Treated as a short-lived secret (TTL 10 min on the
// cloud side) so the value is reasonably mid-entropy.
std::string GeneratePairCode() {
    uint32_t value = 0;
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = ::read(fd, &value, sizeof(value));
        ::close(fd);
        if (n != static_cast<ssize_t>(sizeof(value))) value = 0;
    }
    if (value == 0) {
        std::random_device rd;
        value = (static_cast<uint32_t>(rd()) ^ (static_cast<uint32_t>(rd()) << 1));
    }
    value %= 100000000u;  // 8 decimal digits
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08u", value);
    return buf;
}

std::string FormatPairCodeChunked(const std::string& code) {
    if (code.size() != 8) return code;
    return code.substr(0, 4) + "-" + code.substr(4, 4);
}

// Translate the cloud WS URL into the browser-facing pair page. Examples:
//   wss://my.tenbox.ai/api/device-tunnel -> https://my.tenbox.ai/pair?code=...
//   ws://127.0.0.1:18080/api/device-tunnel -> http://127.0.0.1:18080/pair?code=...
// Allow `TENBOX_PAIR_URL_BASE` to override the host portion in dev when the
// console runs on a different port (e.g. vite at :5173).
std::string PairUrlFor(const std::string& cloud_url, const std::string& code) {
    if (const char* override_base = std::getenv("TENBOX_PAIR_URL_BASE")) {
        if (*override_base) {
            std::string base = override_base;
            while (!base.empty() && base.back() == '/') base.pop_back();
            return base + "/pair?code=" + code;
        }
    }
    WsUrl parsed;
    std::string error;
    if (!ParseWsUrl(cloud_url, &parsed, &error)) {
        return std::string("https://my.tenbox.ai/pair?code=") + code;
    }
    const std::string scheme = parsed.tls ? "https" : "http";
    std::string url = scheme + "://" + parsed.host;
    const bool default_port = (parsed.tls && parsed.port == "443") ||
                               (!parsed.tls && parsed.port == "80");
    if (!default_port) url += ":" + parsed.port;
    url += "/pair?code=" + code;
    return url;
}

std::string ImagesDir(const DaemonConfig& config) {
    return (std::filesystem::path(config.data_dir) / "images").string();
}

nlohmann::json ImageFileToJson(const image_source::ImageFile& file) {
    return {
        {"name", file.name},
        {"url", file.url},
        {"sha256", file.sha256},
        {"size", file.size},
    };
}

nlohmann::json ImageToJson(const image_source::ImageEntry& image, const std::string& images_dir) {
    nlohmann::json files = nlohmann::json::array();
    for (const auto& file : image.files) files.push_back(ImageFileToJson(file));
    return {
        {"id", image.id},
        {"version", image.version},
        {"cache_id", image.CacheId()},
        {"name", image.display_name},
        {"description", image.description},
        {"min_app_version", image.min_app_version},
        {"os", image.os},
        {"arch", image.arch},
        {"platform", image.platform},
        {"size", image.TotalSize()},
        {"cached", image_source::IsImageCached(images_dir, image)},
        {"files", std::move(files)},
    };
}

image_source::ImageEntry ImageFromJson(const nlohmann::json& value) {
    auto parsed = image_source::ParseImages(nlohmann::json{{"images", nlohmann::json::array({value})}}.dump());
    return parsed.empty() ? image_source::ImageEntry{} : parsed.front();
}

std::string FindImageFile(const image_source::ImageEntry& image, const std::string& cache_dir,
                          const std::string& kind) {
    for (const auto& file : image.files) {
        const auto& name = file.name;
        const bool is_kernel = name == "vmlinuz" || name.rfind("vmlinuz", 0) == 0 || name.rfind("Image", 0) == 0;
        const bool is_initrd = name.rfind("initrd", 0) == 0 || name.rfind("initramfs", 0) == 0;
        const bool is_disk = name.find(".qcow2") != std::string::npos || name.find("rootfs") != std::string::npos;
        if ((kind == "kernel" && is_kernel) || (kind == "initrd" && is_initrd) ||
            (kind == "disk" && is_disk)) {
            return (std::filesystem::path(cache_dir) / name).string();
        }
    }
    return {};
}

std::string RandomWsKey() {
    static constexpr char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::random_device rd;
    uint8_t nonce[16] = {};
    for (auto& byte : nonce) byte = static_cast<uint8_t>(rd());

    std::string out;
    out.reserve(24);
    for (size_t i = 0; i < sizeof(nonce); i += 3) {
        const uint32_t b0 = nonce[i];
        const uint32_t b1 = i + 1 < sizeof(nonce) ? nonce[i + 1] : 0;
        const uint32_t b2 = i + 2 < sizeof(nonce) ? nonce[i + 2] : 0;
        const uint32_t value = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(chars[(value >> 18) & 0x3f]);
        out.push_back(chars[(value >> 12) & 0x3f]);
        out.push_back(i + 1 < sizeof(nonce) ? chars[(value >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < sizeof(nonce) ? chars[value & 0x3f] : '=');
    }
    return out;
}

}  // namespace

uint64_t CloudTunnel::CachedDirectorySizeBytes(const std::string& path) const {
    if (path.empty()) return 0;
    // 60s matches the docstring "stale-by-more-than-a-minute" and is a clean
    // multiple of the 30s resources_tick cadence: every other tick walks
    // disk, the alternate tick is served from cache.
    constexpr auto kTtl = std::chrono::seconds(60);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(disk_cache_mu_);
        auto it = disk_cache_.find(path);
        if (it != disk_cache_.end() && now - it->second.at < kTtl) {
            return it->second.bytes;
        }
    }
    const uint64_t bytes = DirectorySizeBytes(path);
    {
        std::lock_guard<std::mutex> lock(disk_cache_mu_);
        disk_cache_[path] = DiskUsageEntry{bytes, now};
    }
    return bytes;
}

void CloudTunnel::EvictDiskCacheEntry(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(disk_cache_mu_);
    disk_cache_.erase(path);
}

CloudTunnel::CloudTunnel(DaemonConfig config, VmStore& store, RuntimeManager& runtime_manager)
    : config_(std::move(config)), store_(store), runtime_manager_(runtime_manager) {
    start_time_seconds_ = UnixNow();
    runtime_manager_.SetCursorCallback([this](const std::string& vm_id, nlohmann::json cursor) {
        PublishRemoteCursor(vm_id, std::move(cursor));
    });
    runtime_manager_.SetAudioCallback([this](const std::string& vm_id, RemoteAudioChunk chunk) {
        PublishRemoteAudio(vm_id, std::move(chunk));
    });
    runtime_manager_.SetStateChangedCallback(
        [this](const std::string& vm_id, const VmRuntimeInfo& info) {
            PushVmStateChanged(vm_id, info);
        });
    runtime_manager_.SetLogAppendCallback(
        [this](const std::string& vm_id, const std::vector<std::string>& lines) {
            EnqueueLogLines(vm_id, lines);
        });
    // Resolved at each VM start so a LlmProxyService restart (e.g.
    // operator changed listen_port via the console) is picked up by the
    // *next* VM boot. VMs that were already running keep the old guestfwd
    // until they reboot; that matches the manager-side behaviour.
    runtime_manager_.SetLlmProxyPortProvider([this]() -> uint16_t {
        return llm_proxy_ ? llm_proxy_->port() : 0;
    });
}

CloudTunnel::~CloudTunnel() {
    runtime_manager_.SetCursorCallback(nullptr);
    runtime_manager_.SetAudioCallback(nullptr);
    runtime_manager_.SetStateChangedCallback(nullptr);
    runtime_manager_.SetLogAppendCallback(nullptr);
    runtime_manager_.SetLlmProxyPortProvider(nullptr);
    Stop();
}

bool CloudTunnel::Start(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(host_settings_mu_);
        host_settings_ = LoadHostSettings(config_.data_dir);
        EnsureLlmProxyForSettingsLocked(host_settings_.llm_proxy);
    }
    // Visible at startup so operators inspecting journalctl can confirm the
    // disk-walk cache is wired up (60s TTL means at most one full walk per
    // VM per minute, regardless of how often the cloud polls).
    std::cerr << "[INFO] disk_usage cache enabled (ttl=60s, evicted on vm.delete)\n";
    if (config_.cloud_url.empty()) return true;
    running_ = true;
    thread_ = std::thread(&CloudTunnel::ThreadMain, this);
    tick_thread_ = std::thread(&CloudTunnel::TickMain, this);
    return true;
}

void CloudTunnel::Stop() {
    running_ = false;
    Disconnect();
    if (thread_.joinable()) thread_.join();
    if (tick_thread_.joinable()) tick_thread_.join();
    if (llm_proxy_) {
        llm_proxy_->Stop();
        llm_proxy_.reset();
    }
}

void CloudTunnel::Disconnect() {
    // Flip the gate first so any background producer that wakes up mid-
    // teardown bails out instead of racing into SSL_write on a half-closed
    // SSL object.
    connected_.store(false);
    if (ssl_) {
        // Best-effort shutdown; SSL_shutdown returning 0 means peer hasn't
        // sent close_notify yet, but we always close the underlying fd next
        // so a half-shut state can't linger.
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    tls_enabled_ = false;
}

bool CloudTunnel::Connect(std::string* error) {
    WsUrl url;
    if (!ParseWsUrl(config_.cloud_url, &url, error)) return false;
    host_id_ = url.host_id.empty() ? LoadOrCreateDeviceId(config_.data_dir) : url.host_id;
    if (url.host_id.empty()) {
        url.target += (url.target.find('?') == std::string::npos ? "?" : "&");
        url.target += "host_id=" + host_id_;
    }

    // Use the system's default address selection (AF_UNSPEC -> RFC 6724
    // ordering in glibc), same as curl / browsers. We briefly defaulted
    // to IPv4-only to work around what looked like an IPv6 reachability
    // problem on a single-board-computer test host; the actual failure
    // there was the tick-thread race fixed in 0.7.14, not IPv6.
    //
    // Operators stuck on broken dual-stack networks (rare: an AAAA route
    // is advertised but doesn't forward) can still pin the family with
    // TENBOX_CLOUD_PREFER_FAMILY=v4 / v6 in the systemd EnvironmentFile.
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (const char* fam = std::getenv("TENBOX_CLOUD_PREFER_FAMILY")) {
        if (std::string_view(fam) == "v4") hints.ai_family = AF_INET;
        else if (std::string_view(fam) == "v6") hints.ai_family = AF_INET6;
        // anything else (including "any") leaves AF_UNSPEC.
    }

    addrinfo* result = nullptr;
    const int gai = ::getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &result);
    if (gai != 0) {
        if (error) *error = gai_strerror(gai);
        return false;
    }

    // Walk every candidate so a transient failure on the first record
    // (e.g. one Cloudflare anycast IP that happens to be flapping) still
    // tries the next. Capture the last connect() errno so the caller can
    // log "tried 1.2.3.4:443 -> ECONNREFUSED" instead of the previous
    // opaque "failed to connect cloud gateway".
    int fd = -1;
    int last_errno = 0;
    char last_addr[64] = {};
    for (auto* rp = result; rp; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            // Stash the address we actually connected to in case TLS or
            // the WS handshake fails later and we want to log it.
            (void)::getnameinfo(rp->ai_addr, rp->ai_addrlen, last_addr,
                                sizeof(last_addr), nullptr, 0, NI_NUMERICHOST);
            break;
        }
        last_errno = errno;
        (void)::getnameinfo(rp->ai_addr, rp->ai_addrlen, last_addr,
                            sizeof(last_addr), nullptr, 0, NI_NUMERICHOST);
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);
    if (fd < 0) {
        if (error) {
            std::ostringstream msg;
            msg << "failed to connect cloud gateway (";
            if (last_addr[0]) msg << last_addr << ":" << url.port << ", ";
            msg << "errno=" << last_errno << " " << std::strerror(last_errno) << ")";
            *error = msg.str();
        }
        return false;
    }
    fd_ = fd;
    tls_enabled_ = url.tls;

    if (tls_enabled_) {
        // One-time process init is implicit on OpenSSL >= 1.1; SSL_CTX_new
        // pulls in the modern auto-init path. Verify against the system CA
        // bundle and pin the SNI/peer hostname so a hostile cloud relay
        // can't impersonate my.tenbox.ai with a different valid cert.
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) {
            Disconnect();
            if (error) *error = "SSL_CTX_new failed";
            return false;
        }
        // Floor at TLS 1.2; let OpenSSL negotiate the highest version the
        // peer supports (typically TLS 1.3 with Cloudflare). We previously
        // also pinned the *max* to 1.2 because tcpdump showed an early
        // Application Data record landing before the client's Finished,
        // which Cloudflare rejected with a fatal `unexpected_message`
        // alert. The real cause was a tick-thread race that called
        // SSL_write() while SSL_connect() was still in flight (see
        // connected_ in cloud_tunnel.h); now that the gate is correct
        // there is no reason to forgo 1.3.
        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(ssl_ctx_);
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
        // Advertise ALPN "http/1.1" on the TLS ClientHello. CF and other
        // edges sometimes treat a missing ALPN as a bot-like fingerprint;
        // browsers + curl always send one. Pinning http/1.1 also keeps
        // us off any future h2/h3 negotiation path we don't speak.
        static const unsigned char kAlpnHttp11[] = {8, 'h','t','t','p','/','1','.','1'};
        SSL_CTX_set_alpn_protos(ssl_ctx_, kAlpnHttp11, sizeof(kAlpnHttp11));
        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) {
            Disconnect();
            if (error) *error = "SSL_new failed";
            return false;
        }
        SSL_set_tlsext_host_name(ssl_, url.host.c_str());
        SSL_set1_host(ssl_, url.host.c_str());
        SSL_set_fd(ssl_, fd_);
        if (SSL_connect(ssl_) != 1) {
            const long verify = SSL_get_verify_result(ssl_);
            unsigned long err = ERR_get_error();
            char buf[256] = {};
            if (err) ERR_error_string_n(err, buf, sizeof(buf));
            std::ostringstream msg;
            msg << "TLS handshake failed";
            if (verify != X509_V_OK) {
                msg << " (verify=" << verify << ": "
                    << X509_verify_cert_error_string(verify) << ")";
            }
            if (buf[0]) msg << " (" << buf << ")";
            Disconnect();
            if (error) *error = msg.str();
            return false;
        }
    }

    std::ostringstream request;
    request << "GET " << url.target << " HTTP/1.1\r\n"
            << "Host: " << url.host;
    const bool default_port = (url.tls && url.port == "443") ||
                               (!url.tls && url.port == "80");
    if (!default_port) request << ":" << url.port;
    // User-Agent identifies us in cloud-side logs and avoids any edge
    // network (Cloudflare bot fight mode etc.) that disfavours
    // UA-less clients. Format follows the tenboxd/<version> convention
    // used by host_updater's apt invocation.
    request << "\r\n"
            << "User-Agent: tenboxd/" << TENBOX_VERSION << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "Sec-WebSocket-Key: " << RandomWsKey() << "\r\n\r\n";
    const std::string req = request.str();
    if (!TransportSend(req.data(), req.size())) {
        Disconnect();
        if (error) *error = "failed to send WebSocket handshake";
        return false;
    }

    std::string response;
    char ch = 0;
    while (response.find("\r\n\r\n") == std::string::npos) {
        if (!TransportRecv(&ch, 1)) {
            Disconnect();
            if (error) *error = "failed to read WebSocket handshake";
            return false;
        }
        response.push_back(ch);
        if (response.size() > 8192) {
            Disconnect();
            if (error) *error = "WebSocket handshake response too large";
            return false;
        }
    }
    if (response.find(" 101 ") == std::string::npos) {
        Disconnect();
        if (error) *error = "cloud gateway did not accept WebSocket upgrade";
        return false;
    }
    // Only now is it safe for the tick thread / log flusher / event
    // callbacks to write WS frames. See cloud_tunnel.h::connected_ for why
    // gating on fd_ alone produced a TLS `unexpected_message` regression.
    connected_.store(true);
    return true;
}

bool CloudTunnel::TransportSend(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        ssize_t n;
        if (tls_enabled_) {
            n = SSL_write(ssl_, p, static_cast<int>(size));
        } else {
            n = ::send(fd_, p, size, MSG_NOSIGNAL);
        }
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool CloudTunnel::TransportRecv(void* data, size_t size) {
    auto* p = static_cast<uint8_t*>(data);
    while (size > 0) {
        ssize_t n;
        if (tls_enabled_) {
            n = SSL_read(ssl_, p, static_cast<int>(size));
        } else {
            n = ::recv(fd_, p, size, 0);
        }
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool CloudTunnel::SendJson(const nlohmann::json& value) {
    std::lock_guard<std::mutex> lock(send_mu_);
    // Guard against background producers (tick thread, log flusher, runtime
    // event callbacks) that race ahead of the TLS+WS handshake. Writing a
    // WebSocket frame onto the SSL session while SSL_connect() is still in
    // flight makes OpenSSL emit it as an early Application Data record,
    // which Cloudflare fatally rejects with `unexpected_message`.
    if (!connected_.load()) return false;
    if (fd_ < 0) return false;
    const std::string payload = value.dump();
    std::vector<uint8_t> frame;
    frame.push_back(0x81);
    if (payload.size() < 126) {
        frame.push_back(static_cast<uint8_t>(0x80 | payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>(payload.size() >> 8));
        frame.push_back(static_cast<uint8_t>(payload.size()));
    } else {
        return false;
    }
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    frame.insert(frame.end(), std::begin(mask), std::end(mask));
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
    }
    return TransportSend(frame.data(), frame.size());
}

bool CloudTunnel::ReadJson(nlohmann::json* value) {
    uint8_t header[2] = {};
    if (!TransportRecv(header, sizeof(header))) return false;
    const uint8_t opcode = header[0] & 0x0f;
    if (opcode == 0x8) return false;
    if (opcode != 0x1) return false;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7f;
    if (len == 126) {
        uint8_t ext[2];
        if (!TransportRecv(ext, sizeof(ext))) return false;
        len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        return false;
    }
    uint8_t mask[4] = {};
    if (masked && !TransportRecv(mask, sizeof(mask))) return false;
    std::string payload(len, '\0');
    if (len > 0 && !TransportRecv(payload.data(), static_cast<size_t>(len))) return false;
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
        }
    }
    auto parsed = nlohmann::json::parse(payload, nullptr, false);
    if (parsed.is_discarded()) return false;
    *value = std::move(parsed);
    return true;
}

void CloudTunnel::ThreadMain() {
    // Exponential backoff for reconnect attempts. Caps at 60s so a flaky
    // network or temporary cloud outage does not keep the journal at the
    // previous 1-attempt-per-second cadence (which on a host with broken
    // IPv6 + AF_UNSPEC produced ~30k log lines/day for nothing). Reset
    // only after the link has been *up* for kBackoffResetThreshold so a
    // pathological connect-then-immediately-disconnect loop still backs
    // off instead of hot-spinning at 1Hz.
    using Clock = std::chrono::steady_clock;
    constexpr auto kBackoffMin = std::chrono::seconds(1);
    constexpr auto kBackoffMax = std::chrono::seconds(60);
    constexpr auto kBackoffResetThreshold = std::chrono::seconds(30);
    auto backoff = kBackoffMin;
    auto bump_backoff = [&]() {
        backoff = std::min(backoff * 2, kBackoffMax);
    };
    // Sleep `delay` but wake early on shutdown so SIGTERM doesn't have
    // to wait out a full 60s slot during the backoff phase.
    auto interruptible_sleep = [this](std::chrono::seconds delay) {
        const auto deadline = Clock::now() + delay;
        while (running_ && Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now());
            const auto slice = std::min<std::chrono::milliseconds>(
                remaining, std::chrono::milliseconds(200));
            std::this_thread::sleep_for(slice);
        }
    };

    while (running_) {
        // Refresh the token snapshot at the top of each connect attempt so
        // a successful pair (which writes device.token mid-session) takes
        // effect on the very next reconnect without a daemon restart.
        {
            std::lock_guard<std::mutex> lock(pair_mu_);
            device_token_ = LoadDeviceToken(config_.data_dir);
            if (device_token_.empty()) {
                if (pair_code_.empty()) pair_code_ = GeneratePairCode();
            } else {
                pair_code_.clear();
            }
        }

        std::string error;
        if (!Connect(&error)) {
            std::cerr << "cloud tunnel connect failed: " << error
                      << " (retrying in " << backoff.count() << "s)\n";
            interruptible_sleep(backoff);
            bump_backoff();
            continue;
        }

        const auto connected_at = Clock::now();
        std::cerr << "cloud tunnel connected as " << host_id_ << "\n";
        SendJson(HelloPayload());
        // Once the WS upgrade succeeds and we know we still need pairing,
        // surface the URL so an operator running `curl ... | sh` sees it
        // immediately in the install transcript without grepping
        // journalctl.
        std::string code_snapshot;
        {
            std::lock_guard<std::mutex> lock(pair_mu_);
            code_snapshot = pair_code_;
        }
        if (!code_snapshot.empty()) {
            const std::string url = PairUrlFor(config_.cloud_url, code_snapshot);
            std::cerr << "[INFO] Visit " << url << " to bind this host\n"
                      << "[INFO] pairing code: " << FormatPairCodeChunked(code_snapshot) << "\n";
        }

        while (running_) {
            nlohmann::json message;
            if (!ReadJson(&message)) break;
            const std::string type = message.value("type", "");
            // Cloud-pushed pairing events are one-way; everything else is a
            // request that expects a response on the same envelope id.
            if (type == "device.paired") {
                HandleDevicePaired(message.value("payload", nlohmann::json::object()));
                continue;
            }
            if (type == "device.pair_invalid") {
                HandleDevicePairInvalid();
                break;  // Reconnect with a fresh code.
            }
            if (type == "device.unauthorized") {
                HandleDeviceUnauthorized(message.value("payload", nlohmann::json::object()));
                break;  // Reconnect; a rejected token may now need a pair code.
            }
            // Most handlers return a reply envelope here. host.update
            // also returns one (a synchronous "accepted" ack) but
            // additionally spawns a worker thread that may push a
            // best-effort apt-failure envelope later on the same id
            // — see HandleHostUpdate for the full rationale.
            auto reply = HandleRequest(message);
            if (!reply.is_null()) {
                SendJson(std::move(reply));
            }
        }

        Disconnect();
        // Reset the backoff only when the session was healthy long enough
        // that the disconnect can plausibly be a one-off (network blip,
        // server restart). Sub-threshold sessions keep escalating so a
        // pathological "WS upgrade succeeds, server kicks us 200ms later"
        // loop doesn't reconnect at 1Hz.
        const auto stayed_up = Clock::now() - connected_at;
        if (stayed_up >= kBackoffResetThreshold) {
            backoff = kBackoffMin;
        } else {
            bump_backoff();
        }
        if (running_) {
            interruptible_sleep(backoff);
        }
    }
}

nlohmann::json CloudTunnel::HelloPayload() const {
    nlohmann::json payload = HostResourcesPayload();
    {
        std::lock_guard<std::mutex> lock(pair_mu_);
        if (!device_token_.empty()) {
            payload["device_token"] = device_token_;
        } else if (!pair_code_.empty()) {
            payload["pair_code"] = pair_code_;
        }
    }
    return {
        {"id", GenerateUuid()},
        {"type", "device.hello"},
        {"host_id", host_id_},
        {"payload", std::move(payload)},
    };
}

void CloudTunnel::HandleDevicePaired(const nlohmann::json& payload) {
    const std::string token = payload.value("device_token", "");
    if (token.empty()) {
        std::cerr << "[WARN] device.paired received without device_token; ignoring\n";
        return;
    }
    std::string error;
    if (!SaveDeviceToken(config_.data_dir, token, &error)) {
        std::cerr << "[ERROR] failed to persist device token: " << error << "\n";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pair_mu_);
        device_token_ = token;
        pair_code_.clear();
    }
    std::cerr << "[INFO] cloud pairing complete for host " << host_id_ << "\n";
}

void CloudTunnel::HandleDevicePairInvalid() {
    std::lock_guard<std::mutex> lock(pair_mu_);
    pair_code_.clear();  // Force a fresh code on the next connect attempt.
}

void CloudTunnel::HandleDeviceUnauthorized(const nlohmann::json& payload) {
    const std::string error = payload.value("error", "");
    const bool token_rejected = error.find("device_token rejected") != std::string::npos ||
                                error.find("re-pair required") != std::string::npos;
    if (!token_rejected) {
        std::cerr << "[WARN] cloud unauthorized: "
                  << (error.empty() ? "no error detail" : error) << "\n";
        return;
    }

    std::string remove_error;
    if (!DeleteDeviceToken(config_.data_dir, &remove_error)) {
        std::cerr << "[ERROR] " << remove_error << "\n";
    }
    {
        std::lock_guard<std::mutex> lock(pair_mu_);
        device_token_.clear();
        pair_code_.clear();  // Force a fresh code on the next connect attempt.
    }
    std::cerr << "[WARN] cloud rejected device token; cleared local token and will re-pair\n";
}

namespace {

bool ProbeH264Encoder(H264Profile profile, PixelFormat pixel_format = PixelFormat::kYuv420p) {
    VideoEncoderConfig config;
    config.width = 256;
    config.height = 256;
    config.bitrate_bps = 500'000;
    config.framerate = 30;
    config.codec = VideoCodec::kH264;
    config.h264_profile = profile;
    config.input_format = pixel_format;

    FfmpegH264VideoEncoder encoder;
    std::string error;
    return encoder.Open(config, &error);
}

// One-shot encoder capability probe. H.264 uses the same candidate-open path
// as the WebRTC encoder so hardware wrappers do not count as usable unless the
// current host can actually open them or fall back to software.
const nlohmann::json& EncoderCapabilitiesCached() {
    static const nlohmann::json kCaps = []() {
        nlohmann::json out = {
            {"h264_high", false},
            {"h264_main", false},
            {"h264_baseline", false},
            {"h264_yuv444", false},
            {"opus", false},
        };
        out["h264_high"] = ProbeH264Encoder(H264Profile::kHigh);
        out["h264_main"] = ProbeH264Encoder(H264Profile::kMain);
        out["h264_baseline"] = ProbeH264Encoder(H264Profile::kConstrainedBaseline);
        out["h264_yuv444"] = ProbeH264Encoder(H264Profile::kHigh, PixelFormat::kYuv444p);
        const AVCodec* opus = avcodec_find_encoder_by_name("libopus");
        if (!opus) opus = avcodec_find_encoder(AV_CODEC_ID_OPUS);
        if (opus) out["opus"] = true;
        return out;
    }();
    return kCaps;
}

}  // namespace

nlohmann::json CloudTunnel::HostResourcesPayload() const {
    // Sum running tenbox VM RSS. Walking `store_.List()` here is cheap (small
    // number of VMs) and avoids a second source of truth for which VMs are
    // running. We deliberately only count `kRunning`: starting/stopping
    // states have unstable pids that the sampler may not have yet.
    uint64_t vm_rss = 0;
    for (const auto& vm : store_.List()) {
        if (vm.runtime.state != VmState::kRunning || vm.runtime.pid <= 0) continue;
        const auto sample = runtime_manager_.SampleProcessResources(vm.spec.vm_id);
        vm_rss += sample.rss_bytes;
    }
    const std::string images_dir = ImagesDir(config_);
    const uint16_t llm_port = llm_proxy_ ? llm_proxy_->port() : 0;
    return {
        {"resources", ToJson(ReadHostResources(config_.data_dir))},
        {"doctor", ToJson(RunKvmDoctor())},
        {"hostname", Hostname()},
        {"data_dir", config_.data_dir},
        {"socket_path", config_.socket_path},
        {"runtime_path", config_.runtime_path},
        {"daemon_version", TENBOX_VERSION},
        {"daemon_uptime_seconds", std::max<int64_t>(0, UnixNow() - start_time_seconds_)},
        {"cloud_connected", connected_.load()},
        {"tenbox_vm_memory_bytes", vm_rss},
        {"image_cache_bytes", CachedDirectorySizeBytes(images_dir)},
        {"encoder_caps", EncoderCapabilitiesCached()},
        // Live LLM proxy port (0 when no mappings are configured). Guests
        // reach it as 10.0.2.2:<port> through the default slirp NAT.
        {"llm_proxy_port", llm_port},
        // Platform identity, surfaced in every tick so the cloud's
        // host.update modal can compare against /api/releases/linux/latest
        // (supported_archs, min_glibc) before the user clicks upgrade.
        {"os_release", host_updater::ReadOsRelease()},
        {"arch", host_updater::BuildArch()},
        {"glibc_version", host_updater::RuntimeGlibcVersion()},
    };
}

nlohmann::json CloudTunnel::GetLlmProxySettings() const {
    std::lock_guard<std::mutex> lock(host_settings_mu_);
    return ToJson(host_settings_.llm_proxy);
}

nlohmann::json CloudTunnel::UpdateLlmProxySettings(const nlohmann::json& payload) {
    auto next = LlmProxyFromJson(payload);
    uint16_t old_port = 0;
    uint16_t new_port = 0;
    HostSettings to_persist;
    {
        std::lock_guard<std::mutex> lock(host_settings_mu_);
        old_port = llm_proxy_ ? llm_proxy_->port() : 0;
        host_settings_.llm_proxy = next;
        to_persist = host_settings_;
        // Hot-load mappings into the running proxy (or spin one up if
        // this is the first non-empty configuration). Done under the
        // settings lock so concurrent gets observe a consistent view.
        EnsureLlmProxyForSettingsLocked(next);
        new_port = llm_proxy_ ? llm_proxy_->port() : 0;
    }
    if (!SaveHostSettings(config_.data_dir, to_persist)) {
        return Error("host_settings_persist_failed", "failed to write host_settings.json");
    }

    // If the bound port moved (start, stop, or relocate) push the new
    // guestfwd to every running VM so it picks up the new endpoint
    // without a reboot. Done outside host_settings_mu_ because
    // ApplyForwards walks the VmStore and talks to the runtime over
    // its control socket, both of which take their own locks.
    if (old_port != new_port) {
        for (const auto& vm : store_.List()) {
            if (vm.runtime.state != VmState::kRunning) continue;
            runtime_manager_.ApplyForwards(vm.spec.vm_id);
        }
    }

    auto out = ToJson(next);
    out["live_listen_port"] = new_port;
    return out;
}

void CloudTunnel::EnsureLlmProxyForSettingsLocked(const LlmProxySettings& desired) {
    // No mappings configured: tear down to release the bound port. The
    // operator may have just cleared every row in the console UI.
    if (desired.mappings.empty()) {
        if (llm_proxy_) {
            llm_proxy_->Stop();
            llm_proxy_.reset();
        }
        return;
    }

    if (!llm_proxy_) {
        llm_proxy_ = std::make_unique<LlmProxyService>(desired, config_.data_dir);
        if (!llm_proxy_->Start()) {
            // Don't keep a dead service around — a future settings update
            // can retry from scratch.
            llm_proxy_.reset();
            std::cerr << "[ERROR] failed to start LLM proxy service\n";
        }
        return;
    }

    // Already running. If the operator changed the listen port we need
    // a stop/start cycle; otherwise hot-reload mappings + logging in
    // place so in-flight requests are not interrupted.
    const uint16_t live = llm_proxy_->port();
    const bool port_changed = desired.listen_port != 0 && desired.listen_port != live;
    if (port_changed) {
        llm_proxy_->Stop();
        llm_proxy_ = std::make_unique<LlmProxyService>(desired, config_.data_dir);
        if (!llm_proxy_->Start()) {
            llm_proxy_.reset();
            std::cerr << "[ERROR] failed to restart LLM proxy on port "
                      << desired.listen_port << "\n";
        }
    } else {
        llm_proxy_->UpdateSettings(desired);
    }
}

nlohmann::json CloudTunnel::VmListPayload() const {
    nlohmann::json vms = nlohmann::json::array();
    for (const auto& vm : store_.List()) {
        auto item = ToJson(vm);
        item["resources"] = {
            {"disk_usage_bytes", CachedDirectorySizeBytes(vm.spec.vm_dir)},
        };
        if (vm.runtime.pid > 0 && vm.runtime.state == VmState::kRunning) {
            item["resources"]["process"] = ToJson(runtime_manager_.SampleProcessResources(vm.spec.vm_id));
        }
        vms.push_back(std::move(item));
    }
    return {{"vms", std::move(vms)}};
}

nlohmann::json CloudTunnel::HandleRequest(const nlohmann::json& request) {
    const std::string id = request.value("id", GenerateUuid());
    const std::string type = request.value("type", "");
    const std::string vm_id = request.value("vm_id", "");
    nlohmann::json payload;

    if (type == "host.resources" || type == "device.capabilities") {
        payload = HostResourcesPayload();
    } else if (type == "image.cached.list") {
        payload = ImageListPayload();
    } else if (type == "image.cached.delete") {
        payload = DeleteCachedImage(request.value("payload", nlohmann::json::object()));
    } else if (type == "image.download.start") {
        payload = StartImageDownload(request.value("payload", nlohmann::json::object()));
    } else if (type == "image.download.list") {
        payload = ImageDownloadList();
    } else if (type == "image.download.status") {
        payload = ImageDownloadStatus(request.value("payload", nlohmann::json::object()));
    } else if (type == "image.download.cancel") {
        payload = CancelImageDownload(request.value("payload", nlohmann::json::object()));
    } else if (type == "remote_session.create") {
        payload = CreateRemoteSession(vm_id, request.value("payload", nlohmann::json::object()));
    } else if (type == "remote_session.resize") {
        payload = ResizeRemoteSession(vm_id, request.value("payload", nlohmann::json::object()));
    } else if (type == "remote_session.close") {
        payload = CloseRemoteSession(vm_id, request.value("payload", nlohmann::json::object()));
    } else if (type == "remote_session.configure" ||
               type.rfind("remote_signal.", 0) == 0) {
        payload = HandleRemoteSignal(vm_id, type, request.value("payload", nlohmann::json::object()));
    } else if (type == "vm.list") {
        payload = VmListPayload();
    } else if (type == "vm.create") {
        payload = CreateVm(request.value("payload", nlohmann::json::object()));
    } else if (type == "vm.edit") {
        payload = EditVm(vm_id, request.value("payload", nlohmann::json::object()));
    } else if (type == "vm.delete") {
        payload = DeleteVm(vm_id);
    } else if (type == "host.llm_proxy.get") {
        payload = GetLlmProxySettings();
    } else if (type == "host.llm_proxy.set") {
        payload = UpdateLlmProxySettings(request.value("payload", nlohmann::json::object()));
    } else if (type == "vm.start") {
        std::string error;
        if (!runtime_manager_.StartVm(vm_id, &error)) {
            // Pull the structured failure code from the store so the cloud
            // sees `kvm_unsupported` / `kernel_missing` / `port_conflict`
            // instead of the generic `vm_start_failed` umbrella.
            std::string code = "vm_start_failed";
            if (auto record = store_.Get(vm_id); record && record->runtime.last_failure) {
                code = record->runtime.last_failure->code;
            }
            return {{"id", id}, {"type", type + ".response"}, {"host_id", host_id_},
                    {"vm_id", vm_id}, {"payload", Error(code, error)}};
        }
        payload = nlohmann::json::object();
    } else if (type == "vm.stop" || type == "vm.shutdown") {
        std::string error;
        if (!runtime_manager_.StopVm(vm_id, &error)) {
            return {{"id", id}, {"type", type + ".response"}, {"host_id", host_id_},
                    {"vm_id", vm_id}, {"payload", Error("vm_stop_failed", error)}};
        }
        payload = nlohmann::json::object();
    } else if (type == "vm.logs.tail") {
        payload = runtime_manager_.Logs(vm_id, request.value("lines", 200));
    } else if (type == "host.update") {
        return HandleHostUpdate(id, request.value("payload", nlohmann::json::object()));
    } else {
        return {{"id", id}, {"type", type + ".response"}, {"host_id", host_id_},
                {"vm_id", vm_id}, {"payload", Error("unknown_request", "unknown cloud request: " + type)}};
    }

    if (payload.is_object() && payload.contains("ok") && payload.value("ok", true) == false) {
        return {{"id", id}, {"type", type + ".response"}, {"host_id", host_id_},
                {"vm_id", vm_id}, {"payload", std::move(payload)}};
    }
    return {{"id", id}, {"type", type + ".response"}, {"host_id", host_id_},
            {"vm_id", vm_id}, {"payload", Ok(std::move(payload))}};
}

nlohmann::json CloudTunnel::HandleHostUpdate(const std::string& id,
                                              const nlohmann::json& payload) {
    constexpr const char* kType = "host.update.response";
    auto build_envelope = [&](nlohmann::json body) {
        return nlohmann::json{
            {"id", id},
            {"type", kType},
            {"host_id", host_id_},
            {"payload", std::move(body)},
        };
    };

    const std::string target_version_preview = payload.value("target_version", "");
    std::cerr << "[INFO] host.update: request received from=" << TENBOX_VERSION
              << " target="
              << (target_version_preview.empty() ? "<latest>" : target_version_preview)
              << " (id=" << id << ")\n";

    // Strict refusal: any VM that is starting / running / stopping /
    // rebooting blocks the upgrade. Plan §4 + decision A: console
    // surfaces the names so the operator knows exactly what to stop.
    auto running = host_updater::CollectRunningVms(store_);
    if (!running.empty()) {
        nlohmann::json vms = nlohmann::json::array();
        std::string names;
        for (const auto& vm : running) {
            vms.push_back({{"vm_id", vm.vm_id}, {"name", vm.name}, {"state", vm.state}});
            if (!names.empty()) names += ", ";
            names += (vm.name.empty() ? vm.vm_id : vm.name) + "(" + vm.state + ")";
        }
        std::cerr << "[WARN] host.update: refused (vms_running): " << names << "\n";
        auto err = Error("vms_running", "one or more VMs are still active; stop them before upgrading");
        err["running_vms"] = std::move(vms);
        return build_envelope(std::move(err));
    }

    // Refuse on hand-built or hand-installed daemons. The cloud
    // upgrader must never touch a binary it doesn't own.
    const auto apt_status = host_updater::CheckAptManaged();
    if (!apt_status.ok) {
        std::cerr << "[WARN] host.update: refused (update_disabled): "
                  << apt_status.reason << "\n";
        return build_envelope(Error("update_disabled", apt_status.reason));
    }

    const std::string target_version = target_version_preview;
    const std::string from_version = TENBOX_VERSION;

    const std::string log_path =
        (std::filesystem::path(config_.data_dir) / "logs" / "update.log").string();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(log_path).parent_path(), ec);

    // Spawn apt as a fully detached process living in its own systemd
    // scope (see SpawnAptUpgrade for the cgroup rationale). The call
    // is non-blocking: the daemon never waits for apt to finish, and
    // the apt process survives our own SIGTERM. The cloud will learn
    // the outcome by observing daemon_version in the next host tick
    // after systemd restarts us; failure modes show up in
    // /var/lib/tenbox/logs/update.log for operator inspection.
    const auto spawn = host_updater::SpawnAptUpgrade(target_version, log_path);
    if (!spawn.ok) {
        std::cerr << "[ERROR] host.update: failed to spawn apt: "
                  << spawn.error << "\n";
        auto err = Error("apt_failed",
                         "failed to spawn apt: " + spawn.error);
        err["from"] = from_version;
        if (!target_version.empty()) err["requested"] = target_version;
        return build_envelope(std::move(err));
    }
    std::cerr << "[INFO] host.update: spawned apt in detached scope "
              << "(wrapper pid=" << spawn.pid << ", log=" << log_path
              << "); ack sent to cloud, awaiting daemon restart from postinst\n";

    // Synchronous ack: tell the console "I've accepted the request and
    // started apt; switch to polling daemon_version for the real
    // outcome." Includes `to` so the console knows which version to
    // expect in the post-restart tick.
    return build_envelope(Ok({
        {"accepted", true},
        {"from", from_version},
        {"to", target_version.empty() ? std::string() : target_version},
    }));
}

nlohmann::json CloudTunnel::ImageListPayload() const {
    const auto images_dir = ImagesDir(config_);
    nlohmann::json images = nlohmann::json::array();
    for (const auto& image : image_source::GetCachedImages(images_dir)) {
        images.push_back(ImageToJson(image, images_dir));
    }
    return {{"images_dir", images_dir}, {"images", std::move(images)}};
}

nlohmann::json CloudTunnel::DeleteCachedImage(const nlohmann::json& payload) {
    const std::string cache_id = payload.value("cache_id", "");
    if (cache_id.empty()) return Error("image_invalid", "cache_id is required");

    const auto images_dir = ImagesDir(config_);
    for (const auto& image : image_source::GetCachedImages(images_dir)) {
        if (image.CacheId() != cache_id) continue;

        std::lock_guard<std::mutex> lock(download_mu_);
        for (const auto& [id, job] : download_jobs_) {
            (void)id;
            if (job.cache_id == cache_id && (job.status == "running" || job.status == "pending")) {
                return Error("image_busy", "image download is still running");
            }
        }

        const auto cache_dir = image_source::ImageCacheDir(images_dir, image);
        std::error_code ec;
        const auto removed = std::filesystem::remove_all(cache_dir, ec);
        if (ec) return Error("image_delete_failed", "failed to delete cached image: " + ec.message());
        PushImageCachedRemoved(cache_id);
        return {{"cache_id", cache_id}, {"removed_entries", removed}};
    }
    return Error("image_not_found", "cached image not found");
}

nlohmann::json CloudTunnel::StartImageDownload(const nlohmann::json& payload) {
    const auto image = ImageFromJson(payload.value("image", nlohmann::json::object()));
    if (image.id.empty() || image.files.empty()) return Error("image_invalid", "invalid image metadata");

    // Hard guard against host/image platform mismatch. Without this, a buggy
    // or stale console can hand us an x86_64 rootfs on an arm64 host, and we
    // happily download ~1.6 GB only to fail at boot with "invalid ARM64 Image
    // magic". Platform identity comes from the image manifest (authoritative)
    // and is matched against the host's compiled-in arch.
    const std::string host_platform = image_source::HostPlatform();
    const std::string image_platform = image_source::NormalizePlatform(image.platform);
    if (image_platform != host_platform) {
        return Error("image_arch_mismatch",
                     "image platform '" + image_platform +
                         "' does not match host platform '" + host_platform + "'");
    }

    const std::string images_dir = ImagesDir(config_);
    if (image_source::IsImageCached(images_dir, image)) {
        return {{"job_id", ""}, {"cache_id", image.CacheId()}, {"status", "done"}};
    }

    const std::string job_id = GenerateUuid();
    {
        std::lock_guard<std::mutex> lock(download_mu_);
        for (const auto& [id, job] : download_jobs_) {
            if (job.cache_id == image.CacheId() && (job.status == "running" || job.status == "pending")) {
                return {{"job_id", id}, {"cache_id", job.cache_id}, {"status", job.status}};
            }
        }
        DownloadJob job;
        job.job_id = job_id;
        job.cache_id = image.CacheId();
        job.image_name = image.display_name;
        job.status = "running";
        job.total_bytes = image.TotalSize();
        job.started_at = UnixNow();
        job.updated_at = job.started_at;
        download_jobs_[job_id] = job;
    }

    std::thread([this, image, job_id, images_dir]() {
        const std::string cache_dir = image_source::ImageCacheDir(images_dir, image);
        const auto started = std::chrono::steady_clock::now();
        // Browser-side progress is rate-limited to ~2Hz to avoid flooding the
        // WS with envelope traffic when curl reports tiny deltas faster than
        // the user can perceive.
        auto last_progress_push = std::chrono::steady_clock::now() -
                                  std::chrono::seconds(1);
        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);
        bool ok = !ec;
        std::string error = ec ? ec.message() : "";
        uint64_t done = 0;
        for (const auto& file : image.files) {
            if (!ok) break;
            const auto dest = std::filesystem::path(cache_dir) / file.name;
            const auto tmp = dest.string() + ".tmp";
            {
                std::lock_guard<std::mutex> lock(download_mu_);
                auto& job = download_jobs_[job_id];
                job.status = "running";
                job.downloaded_bytes = done;
                job.updated_at = UnixNow();
                if (job.cancel_requested) {
                    ok = false;
                    error = "cancelled";
                    break;
                }
            }
            const pid_t pid = ::fork();
            if (pid == 0) {
                ::execlp("curl", "curl", "-L", "-f", "--retry", "2", "-o", tmp.c_str(), file.url.c_str(), nullptr);
                _exit(127);
            }
            if (pid < 0) {
                ok = false;
                error = "failed to start curl";
                break;
            }
            {
                std::lock_guard<std::mutex> lock(download_mu_);
                download_jobs_[job_id].current_pid = pid;
            }
            int status = 0;
            while (true) {
                const pid_t waited = ::waitpid(pid, &status, WNOHANG);
                if (waited == pid) break;
                if (waited < 0) {
                    ok = false;
                    error = "failed to wait for curl";
                    break;
                }
                bool cancel = false;
                DownloadJob progress_snapshot;
                bool progress_due = false;
                {
                    std::lock_guard<std::mutex> lock(download_mu_);
                    auto& job = download_jobs_[job_id];
                    cancel = job.cancel_requested;
                    std::error_code size_ec;
                    const auto partial = std::filesystem::exists(tmp, size_ec)
                        ? std::filesystem::file_size(tmp, size_ec)
                        : 0;
                    const auto downloaded = std::min<uint64_t>(job.total_bytes, done + (size_ec ? 0 : partial));
                    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                    job.downloaded_bytes = downloaded;
                    job.speed_bps = elapsed > 0 ? static_cast<uint64_t>(downloaded / elapsed) : 0;
                    job.eta_seconds = job.speed_bps > 0 && job.total_bytes > downloaded
                        ? (job.total_bytes - downloaded) / job.speed_bps
                        : 0;
                    job.updated_at = UnixNow();
                    const auto wall_now = std::chrono::steady_clock::now();
                    if (wall_now - last_progress_push >= std::chrono::milliseconds(500)) {
                        progress_snapshot = job;
                        progress_due = true;
                        last_progress_push = wall_now;
                    }
                }
                if (progress_due) PushDownloadProgress(progress_snapshot);
                if (cancel) {
                    ::kill(pid, SIGTERM);
                    ::waitpid(pid, &status, 0);
                    ok = false;
                    error = "cancelled";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            {
                std::lock_guard<std::mutex> lock(download_mu_);
                download_jobs_[job_id].current_pid = -1;
                if (download_jobs_[job_id].cancel_requested) {
                    ok = false;
                    error = "cancelled";
                }
            }
            if (ok) ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            if (!ok) {
                if (error.empty()) error = "failed to download " + file.name;
                break;
            }
            std::filesystem::rename(tmp, dest, ec);
            if (ec) {
                error = "failed to store " + file.name + ": " + ec.message();
                ok = false;
                break;
            }
            done += file.size;
        }
        if (ok) {
            image_source::SaveImageMeta(cache_dir, image);
        } else {
            // Download failed or was cancelled. Clean up so we don't leave
            // half-finished `.tmp` shards (or fully downloaded files for an
            // image that never got an `image.json`) consuming disk space.
            // Without this, the cache_dir would silently accumulate orphans
            // that `IsImageCached` rejects and `GetCachedImages` ignores —
            // invisible to the UI but visible on `du`.
            std::error_code cleanup_ec;
            std::filesystem::remove_all(cache_dir, cleanup_ec);
        }
        DownloadJob terminal_snapshot;
        bool emit_cached_added = false;
        std::string cached_added_id;
        std::string cached_added_name;
        {
            std::lock_guard<std::mutex> lock(download_mu_);
            auto& job = download_jobs_[job_id];
            job.status = ok ? "done" : (error == "cancelled" ? "cancelled" : "failed");
            job.error = error;
            job.downloaded_bytes = ok ? job.total_bytes : done;
            job.speed_bps = 0;
            job.eta_seconds = 0;
            job.updated_at = UnixNow();
            terminal_snapshot = job;
            if (ok) {
                emit_cached_added = true;
                cached_added_id = job.cache_id;
                cached_added_name = job.image_name;
            }
            if (job.status == "cancelled") {
                download_jobs_.erase(job_id);
            }
        }
        PushDownloadTerminal(terminal_snapshot);
        if (emit_cached_added) PushImageCachedAdded(cached_added_id, cached_added_name);
    }).detach();

    return {{"job_id", job_id}, {"cache_id", image.CacheId()}, {"status", "running"}};
}

nlohmann::json CloudTunnel::DownloadJobToJson(const DownloadJob& job) const {
    return {
        {"job_id", job.job_id},
        {"cache_id", job.cache_id},
        {"image_name", job.image_name},
        {"status", job.status},
        {"error", job.error},
        {"downloaded_bytes", job.downloaded_bytes},
        {"total_bytes", job.total_bytes},
        {"speed_bps", job.speed_bps},
        {"eta_seconds", job.eta_seconds},
        {"started_at", job.started_at},
        {"updated_at", job.updated_at},
    };
}

nlohmann::json CloudTunnel::ImageDownloadList() const {
    std::lock_guard<std::mutex> lock(download_mu_);
    nlohmann::json jobs = nlohmann::json::array();
    for (const auto& [id, job] : download_jobs_) {
        (void)id;
        if (job.status == "done" || job.status == "cancelled") continue;
        jobs.push_back(DownloadJobToJson(job));
    }
    return {{"jobs", std::move(jobs)}};
}

nlohmann::json CloudTunnel::ImageDownloadStatus(const nlohmann::json& payload) {
    const std::string job_id = payload.value("job_id", "");
    std::lock_guard<std::mutex> lock(download_mu_);
    auto it = download_jobs_.find(job_id);
    if (it == download_jobs_.end()) return Error("download_not_found", "download job not found");
    auto result = DownloadJobToJson(it->second);
    if ((it->second.status == "done" || it->second.status == "cancelled") && it->second.current_pid <= 0) {
        download_jobs_.erase(it);
    }
    return result;
}

nlohmann::json CloudTunnel::CancelImageDownload(const nlohmann::json& payload) {
    const std::string job_id = payload.value("job_id", "");
    std::lock_guard<std::mutex> lock(download_mu_);
    auto it = download_jobs_.find(job_id);
    if (it == download_jobs_.end()) return Error("download_not_found", "download job not found");
    auto& job = it->second;
    if (job.status == "done" || job.status == "failed" || job.status == "cancelled") {
        auto result = nlohmann::json{{"job_id", job.job_id}, {"status", job.status}};
        if (job.current_pid <= 0) download_jobs_.erase(it);
        return result;
    }
    job.cancel_requested = true;
    job.status = "cancelled";
    job.error = "cancelled";
    if (job.current_pid > 0) ::kill(job.current_pid, SIGTERM);
    return {{"job_id", job.job_id}, {"status", job.status}};
}

std::shared_ptr<WebRtcPeer> CloudTunnel::CreateRemotePeer(
    const std::string& session_id,
    const std::string& vm_id,
    PixelFormat preferred_video_format) {
    auto peer = CreateWebRtcPeer(
        [this, vm_id](RemoteVideoFrame* frame, bool need_full,
                      std::chrono::milliseconds wait_timeout) {
            if (!frame) return false;
            return runtime_manager_.ReadRemoteFrame(vm_id, frame, need_full, wait_timeout);
        },
        preferred_video_format);
    if (peer) {
        peer->SetDataChannelHandler([this, vm_id](const nlohmann::json& message) {
            HandleDataChannelMessage(vm_id, message);
        });
        // Trickle host-side ICE candidates back to the browser through the
        // existing `remote_signal.candidate` cloud relay envelope. The
        // browser-side hostBus router (`remote_signal.candidate` listener
        // in RemoteDesktopPanel) feeds them to RTCPeerConnection.addIceCandidate.
        // Without this, AcceptOffer's softened gathering window would only
        // ship whatever candidates were collected within ~10s; on STUN-blocked
        // networks the answer sometimes returns with zero candidates and
        // the browser would have nothing to connect to.
        // Tear the session down when the WebRTC link reaches a terminal
        // state (libdatachannel reports Failed after its ~30s ICE consent
        // timer, or Closed when the remote ends the DTLS association).
        // Without this hook the daemon kept the `remote_peers_` entry
        // alive forever after a network drop or a tab crash, so a fresh
        // browser tab would always hit `remote_session_conflict` and the
        // video pump kept burning CPU encoding for nobody.
        //
        // We MUST defer the actual cleanup off the libdatachannel worker
        // thread: erasing remote_peers_[session_id] runs ~PeerConnection
        // synchronously, and the callback we're standing in is invoked
        // *from* that PeerConnection. Joining its own threads from
        // inside its callback deadlocks. A detached worker keeps the
        // peer alive until the callback returns, then performs the
        // removal on a fresh stack.
        peer->SetPeerClosedHandler([this, session_id, vm_id](std::string reason) {
            std::fprintf(stdout,
                         "[INFO]  cloud_tunnel: tearing down remote session %s on vm %s (reason=%s)\n",
                         session_id.c_str(), vm_id.c_str(), reason.c_str());
            std::fflush(stdout);
            std::thread([this, session_id, vm_id, reason]() {
                if (fd_ >= 0) {
                    (void)SendJson({
                        {"id", GenerateUuid()},
                        {"type", "remote_session.closed"},
                        {"host_id", host_id_},
                        {"vm_id", vm_id},
                        {"session_id", session_id},
                        {"payload", {
                            {"session_id", session_id},
                            {"reason", reason},
                            {"message", reason == "peer_failed"
                                ? "remote desktop connection failed"
                                : "remote desktop connection closed"},
                        }},
                    });
                }
                {
                    std::lock_guard<std::mutex> lock(remote_peers_mu_);
                    remote_peers_.erase(session_id);
                }
                runtime_manager_.ClearClipboardCallback(vm_id);
                (void)remote_sessions_.Close(vm_id, session_id);
            }).detach();
        });
        peer->SetLocalIceCandidateHandler(
            [this, session_id, vm_id](nlohmann::json candidate) {
                if (fd_ < 0) return;
                nlohmann::json payload = {
                    {"session_id", session_id},
                };
                for (auto it = candidate.begin(); it != candidate.end(); ++it) {
                    payload[it.key()] = it.value();
                }
                (void)SendJson({
                    {"id", GenerateUuid()},
                    {"type", "remote_signal.candidate"},
                    {"host_id", host_id_},
                    {"vm_id", vm_id},
                    {"session_id", session_id},
                    {"payload", std::move(payload)},
                });
            });
        // When the control DC opens, push the latest cached cursor so the
        // browser is in sync even if MOVE_CURSOR events fired (and were
        // dropped) while the channel was still in DTLS/SCTP setup.
        // Source-side dedup in virtio_gpu means we cannot rely on the next
        // mouse motion to deliver state to a late-attaching peer.
        peer->SetDataChannelOpenHandler([this, session_id, vm_id](const std::string& label) {
            if (label != "control") return;
            nlohmann::json cursor = runtime_manager_.CursorSnapshot(vm_id);
            if (!cursor.is_object() || cursor.empty()) return;
            std::shared_ptr<WebRtcPeer> p;
            {
                std::lock_guard<std::mutex> lock(remote_peers_mu_);
                auto it = remote_peers_.find(session_id);
                if (it != remote_peers_.end()) p = it->second;
            }
            if (!p) return;
            (void)p->SendOnDataChannel("control", nlohmann::json{
                {"type", "cursor"},
                {"cursor", std::move(cursor)},
            }.dump());
        });
        // Bridge guest-originated clipboard events into the browser by
        // looking up the peer that owns this session_id and writing a JSON
        // text frame onto its `control` data channel. We intentionally
        // capture session_id (not the peer) so a tear-down race doesn't
        // leave a callback firing into a destroyed peer.
        runtime_manager_.SetClipboardCallback(vm_id,
            [this, session_id](const std::string& /*vm_id*/,
                                const RuntimeManager::ClipboardEvent& event) {
                std::shared_ptr<WebRtcPeer> p;
                {
                    std::lock_guard<std::mutex> lock(remote_peers_mu_);
                    auto it = remote_peers_.find(session_id);
                    if (it != remote_peers_.end()) p = it->second;
                }
                if (!p) return;
                nlohmann::json out = {
                    {"type", event.type},
                    {"selection", event.selection},
                };
                if (event.type == "clipboard.grab") {
                    nlohmann::json types = nlohmann::json::array();
                    for (uint32_t t : event.available_types) {
                        const char* mime = VdAgentTypeToMime(t);
                        if (mime) types.push_back(mime);
                    }
                    out["types"] = std::move(types);
                } else if (event.type == "clipboard.data") {
                    const char* mime = VdAgentTypeToMime(event.data_type);
                    if (!mime) return;
                    out["mime"] = mime;
                    out["bytes_base64"] = Base64Encode(event.data);
                } else if (event.type == "clipboard.request") {
                    const char* mime = VdAgentTypeToMime(event.data_type);
                    if (!mime) return;
                    out["mime"] = mime;
                }
                p->SendOnDataChannel("control", out.dump());
            });
    }
    return peer;
}

void CloudTunnel::HandleDataChannelMessage(const std::string& vm_id, const nlohmann::json& message) {
    if (!message.is_object()) return;
    const std::string type = message.value("type", "");
    if (type.empty()) return;
    // Input fast-path: pointer/wheel arrive on `input-fast` (best-effort);
    // key/pointer.button arrive on `control` (reliable). Either way both
    // dispatch to the same RuntimeManager IPC.
    if (type == "pointer") {
        runtime_manager_.SendPointerEvent(
            vm_id,
            message.value("x", 0),
            message.value("y", 0),
            message.value("buttons", static_cast<uint32_t>(0)));
    } else if (type == "wheel") {
        runtime_manager_.SendWheelEvent(vm_id, message.value("delta", 0));
    } else if (type == "key") {
        runtime_manager_.SendKeyEvent(
            vm_id,
            message.value("key_code", static_cast<uint32_t>(0)),
            message.value("pressed", false));
    } else if (type == "clipboard.grab") {
        const uint32_t selection = message.value("selection", static_cast<uint32_t>(0));
        std::vector<uint32_t> codes;
        if (message.contains("types") && message["types"].is_array()) {
            for (const auto& entry : message["types"]) {
                if (!entry.is_string()) continue;
                const uint32_t code = MimeToVdAgentType(entry.get<std::string>());
                if (code) codes.push_back(code);
            }
        }
        if (!codes.empty()) runtime_manager_.SendClipboardGrabToGuest(vm_id, selection, codes);
    } else if (type == "clipboard.request") {
        const uint32_t selection = message.value("selection", static_cast<uint32_t>(0));
        const uint32_t code = MimeToVdAgentType(message.value("mime", std::string()));
        if (code) runtime_manager_.SendClipboardRequestToGuest(vm_id, selection, code);
    } else if (type == "clipboard.data") {
        const uint32_t selection = message.value("selection", static_cast<uint32_t>(0));
        const uint32_t code = MimeToVdAgentType(message.value("mime", std::string()));
        if (!code) return;
        std::vector<uint8_t> bytes = Base64Decode(message.value("bytes_base64", std::string()));
        runtime_manager_.SendClipboardDataToGuest(vm_id, selection, code, bytes);
    } else if (type == "clipboard.release") {
        const uint32_t selection = message.value("selection", static_cast<uint32_t>(0));
        runtime_manager_.SendClipboardReleaseToGuest(vm_id, selection);
    } else if (type == "vm.reboot" || type == "vm.shutdown") {
        // Guest-OS facing operations: must travel end-to-end over the
        // already-authenticated WebRTC `control` channel rather than the
        // cloud HTTP path, so the cloud never sees plaintext intent.
        std::string error;
        const bool ok = (type == "vm.reboot")
            ? runtime_manager_.RebootVm(vm_id, &error)
            : runtime_manager_.ShutdownVm(vm_id, &error);
        std::shared_ptr<WebRtcPeer> peer;
        if (auto session = remote_sessions_.GetByVm(vm_id)) {
            std::lock_guard<std::mutex> lock(remote_peers_mu_);
            auto it = remote_peers_.find(session->session_id);
            if (it != remote_peers_.end()) peer = it->second;
        }
        if (peer) {
            nlohmann::json ack = {
                {"type", type + ".ack"},
                {"vm_id", vm_id},
                {"ok", ok},
            };
            if (!ok) ack["error"] = error;
            (void)peer->SendOnDataChannel("control", ack.dump());
        }
    } else {
        std::fprintf(stdout,
                     "[DEBUG] cloud_tunnel: unhandled dc message type=%s vm=%s\n",
                     type.c_str(),
                     vm_id.c_str());
        std::fflush(stdout);
    }
}

nlohmann::json CloudTunnel::CreateRemoteSession(const std::string& vm_id, const nlohmann::json& payload) {
    if (vm_id.empty()) return Error("remote_session_invalid", "vm_id is required");
    auto record = store_.Get(vm_id);
    if (!record) return Error("vm_not_found", "VM not found");
    // Allow opening the remote session as soon as the VM has started booting.
    // The WebRTC pump will block on the runtime's framebuffer CV until the
    // first slice arrives, so the user sees the boot splash live instead of
    // waiting for the VM to reach `running`.
    if (record->runtime.state != VmState::kRunning &&
        record->runtime.state != VmState::kStarting) {
        return Error("vm_not_running", "VM must be running or starting before opening a remote session");
    }
    const std::string owner = payload.value("owner_user_id", "cloud");
    const bool force = payload.value("force", false);
    if (force) {
        if (auto existing = remote_sessions_.GetByVm(vm_id)) {
            const std::string old_session_id = existing->session_id;
            {
                std::lock_guard<std::mutex> lock(remote_peers_mu_);
                remote_peers_.erase(old_session_id);
            }
            runtime_manager_.ClearClipboardCallback(vm_id);
            // Notify the displaced viewer so it can tear down its
            // RTCPeerConnection and surface a "session was taken over"
            // banner instead of sitting on a frozen frame waiting for
            // video that will never arrive. Without this push, the old
            // browser's WS stays open, no business event fires, and the
            // panel only ever sees an `iceConnectionState=disconnected`
            // that the UI today silently swallows into the log.
            if (fd_ >= 0) {
                (void)SendJson({
                    {"id", GenerateUuid()},
                    {"type", "remote_session.closed"},
                    {"host_id", host_id_},
                    {"vm_id", vm_id},
                    {"session_id", old_session_id},
                    {"payload", {
                        {"session_id", old_session_id},
                        {"reason", "superseded"},
                        {"message", "another viewer took over this session"},
                    }},
                });
            }
        }
    }
    auto session = remote_sessions_.Create(vm_id, owner, force);
    if (!session) return Error("remote_session_conflict", "VM already has an active remote session");
    const uint32_t display_width = AlignDisplaySize(payload.value("width", static_cast<uint32_t>(1280)));
    const uint32_t display_height = AlignDisplaySize(payload.value("height", static_cast<uint32_t>(720)));
    const uint32_t video_bitrate_bps = ClampVideoBitrate(payload.value("video_bitrate_bps", static_cast<uint32_t>(4'000'000)));
    const PixelFormat video_pixel_format = ParseRemoteVideoPixelFormat(payload);
    (void)runtime_manager_.SetDisplaySize(vm_id, display_width, display_height);
    (void)runtime_manager_.SetRemoteVideoPixelFormat(vm_id, video_pixel_format);
    {
        std::lock_guard<std::mutex> lock(remote_peers_mu_);
        auto peer = CreateRemotePeer(session->session_id, vm_id, video_pixel_format);
        peer->SetVideoBitrate(video_bitrate_bps);
        remote_peers_[session->session_id] = std::move(peer);
    }
    auto json = ToJson(*session);
    json["video_bitrate_bps"] = video_bitrate_bps;
    json["video_pixel_format"] = RemoteVideoPixelFormatName(video_pixel_format);
    // Advertise the same ICE server list the daemon itself is using so
    // both peers agree on the candidate-gathering servers (see
    // ResolvedIceServers for why the STUN defaults skew toward
    // CN-reachable hosts; operators can override via TENBOX_ICE_SERVERS
    // JSON or the legacy TENBOX_STUN_SERVERS comma list). In the
    // self-hosted deployment path the daemon advertises this list
    // verbatim; in the managed cloud path the control-plane rewrites
    // the payload downstream to inject per-user TURN credentials (see
    // tenbox-cloud/docs/turn-rollout.md).
    nlohmann::json ice_servers = nlohmann::json::array();
    for (const auto& spec : ResolvedIceServers()) {
        nlohmann::json entry;
        nlohmann::json urls = nlohmann::json::array();
        for (const auto& url : spec.urls) urls.push_back(url);
        entry["urls"] = std::move(urls);
        if (!spec.username.empty()) entry["username"] = spec.username;
        if (!spec.credential.empty()) entry["credential"] = spec.credential;
        ice_servers.push_back(std::move(entry));
    }
    json["ice_servers"] = std::move(ice_servers);
    // Public snapshot: scrub cursor pixels / clipboard / audio so the cloud
    // relay never observes guest-visible content. Browser receives those
    // exclusively through the WebRTC control DataChannel.
    json["runtime"] = runtime_manager_.RemoteRuntimeSnapshot(
        vm_id, RuntimeManager::SnapshotScope::kPublic);
    json["webrtc_ready"] = NativeWebRtcAvailable();
    json["message"] = NativeWebRtcAvailable()
        ? "remote session signaling is ready"
        : "remote session signaling is ready; native WebRTC media is not attached yet";
    return json;
}

nlohmann::json CloudTunnel::ResizeRemoteSession(const std::string& vm_id, const nlohmann::json& payload) {
    const std::string session_id = payload.value("session_id", "");
    auto session = remote_sessions_.GetByVm(vm_id);
    if (!session || session->session_id != session_id) {
        return Error("remote_session_not_found", "remote session not found");
    }
    const uint32_t width = AlignDisplaySize(payload.value("width", static_cast<uint32_t>(1280)));
    const uint32_t height = AlignDisplaySize(payload.value("height", static_cast<uint32_t>(720)));
    if (!runtime_manager_.SetDisplaySize(vm_id, width, height)) {
        return Error("runtime_not_attached", "failed to resize runtime display");
    }
    return {{"session_id", session_id}, {"width", width}, {"height", height}};
}

nlohmann::json CloudTunnel::CloseRemoteSession(const std::string& vm_id, const nlohmann::json& payload) {
    const std::string session_id = payload.value("session_id", "");
    if (!remote_sessions_.Close(vm_id, session_id)) {
        return Error("remote_session_not_found", "remote session not found");
    }
    {
        std::lock_guard<std::mutex> lock(remote_peers_mu_);
        remote_peers_.erase(session_id);
    }
    // Drop the per-VM clipboard subscriber so a future session can re-attach
    // without inheriting a stale capture.
    runtime_manager_.ClearClipboardCallback(vm_id);
    return {{"session_id", session_id}, {"closed", true}};
}

nlohmann::json CloudTunnel::HandleRemoteSignal(
    const std::string& vm_id,
    const std::string& type,
    const nlohmann::json& payload) {
    const std::string session_id = payload.value("session_id", "");
    auto session = remote_sessions_.GetByVm(vm_id);
    if (!session || session->session_id != session_id) {
        return Error("remote_session_not_found", "remote session not found");
    }
    if (type == "remote_signal.offer") {
        std::shared_ptr<WebRtcPeer> peer;
        {
            std::lock_guard<std::mutex> lock(remote_peers_mu_);
            auto it = remote_peers_.find(session_id);
            if (it == remote_peers_.end()) {
                it = remote_peers_.emplace(session_id, CreateRemotePeer(session_id, vm_id)).first;
            }
            peer = it->second;
        }
        auto answer = peer->AcceptOffer(payload.value("sdp", ""));
        return {
            {"session_id", session_id},
            {"type", "answer"},
            {"sdp", answer.sdp},
            {"candidates", answer.candidates},
            {"webrtc_ready", answer.ok},
            {"message", answer.ok ? "answer created" : answer.error},
        };
    }
    if (type == "remote_signal.candidate") {
        std::string error;
        bool accepted = true;
        std::shared_ptr<WebRtcPeer> peer;
        {
            std::lock_guard<std::mutex> lock(remote_peers_mu_);
            auto it = remote_peers_.find(session_id);
            if (it != remote_peers_.end()) peer = it->second;
        }
        if (peer) accepted = peer->AddIceCandidate(payload, &error);
        if (!accepted) return Error("candidate_rejected", error.empty() ? "failed to add ICE candidate" : error);
        return {{"session_id", session_id}, {"accepted", true}};
    }
    if (type == "remote_session.configure") {
        const uint32_t video_bitrate_bps = ClampVideoBitrate(
            payload.value("video_bitrate_bps", static_cast<uint32_t>(4'000'000)));
        std::string video_pixel_format_name;
        if (payload.contains("video_pixel_format")) {
            const PixelFormat video_pixel_format = ParseRemoteVideoPixelFormat(payload);
            (void)runtime_manager_.SetRemoteVideoPixelFormat(vm_id, video_pixel_format);
            video_pixel_format_name = RemoteVideoPixelFormatName(video_pixel_format);
        }
        // remote.configure fires every time the browser opens a session and
        // again on each window resize / quality change; treat it as debug.
        // Set TENBOX_WEBRTC_VERBOSE=1 to surface it.
        if (const char* v = std::getenv("TENBOX_WEBRTC_VERBOSE");
            v && v[0] != '\0' && std::string_view(v) != "0") {
            std::cout << "[INFO]  cloud_tunnel: remote configure session=" << session_id
                      << " video_bitrate=" << video_bitrate_bps;
            if (!video_pixel_format_name.empty()) {
                std::cout << " video_pixel_format=" << video_pixel_format_name;
            }
            std::cout << "\n";
        }
        std::shared_ptr<WebRtcPeer> peer;
        {
            std::lock_guard<std::mutex> lock(remote_peers_mu_);
            auto it = remote_peers_.find(session_id);
            if (it != remote_peers_.end()) peer = it->second;
        }
        if (peer) peer->SetVideoBitrate(video_bitrate_bps);
        nlohmann::json result = {
            {"session_id", session_id},
            {"video_bitrate_bps", video_bitrate_bps},
        };
        if (!video_pixel_format_name.empty()) {
            result["video_pixel_format"] = video_pixel_format_name;
        }
        return result;
    }
    return Error("signal_invalid", "unknown remote signal: " + type);
}

void CloudTunnel::PublishRemoteCursor(const std::string& vm_id, nlohmann::json cursor) {
    auto session = remote_sessions_.GetByVm(vm_id);
    if (!session) return;

    // Cursor frames piggyback on the per-session WebRTC `control` DataChannel
    // rather than the cloud websocket: it's a direct host<->browser path so
    // it sidesteps the cloud relay's bandwidth budget and stays consistent
    // with how clipboard.* events are delivered. Source-side dedup
    // (virtio_gpu) already guarantees we only get here when the cursor
    // actually changed, so we don't repeat the comparison here.
    std::shared_ptr<WebRtcPeer> peer;
    {
        std::lock_guard<std::mutex> lock(remote_peers_mu_);
        auto it = remote_peers_.find(session->session_id);
        if (it != remote_peers_.end()) peer = it->second;
    }
    if (!peer) return;
    nlohmann::json out = {
        {"type", "cursor"},
        {"cursor", std::move(cursor)},
    };
    (void)peer->SendOnDataChannel("control", out.dump());
}

void CloudTunnel::TickMain() {
    // Push host memory/disk/load and per-VM disk/RSS on a slow schedule so
    // tenboxd stays light: host metrics are cheap; VmResourcesSnapshot walks
    // each VM directory for disk usage and is intentionally less frequent.
    // VM lifecycle state is already pushed asynchronously via vm.state_changed.
    using Clock = std::chrono::steady_clock;
    constexpr auto kHostResourcesInterval = std::chrono::seconds(30);
    constexpr auto kVmResourcesInterval = std::chrono::seconds(30);

    // Fire the first tick as soon as the WS link is up: the values reported in
    // `device.hello` are by definition daemon-startup snapshots (uptime ~0,
    // running-VM RSS ~0), so without an immediate refresh the browser shows
    // stale numbers until the next 30s boundary. We re-arm to "now" whenever
    // we observe an fd_ transition so reconnects also push a fresh snapshot.
    auto next_host = Clock::now();
    auto next_vm = Clock::now() + kVmResourcesInterval;
    bool last_connected = connected_.load();

    while (running_) {
        const auto now = Clock::now();
        const bool is_connected = connected_.load();

        if (!last_connected && is_connected) {
            // Tunnel just (re)connected — refresh the host snapshot
            // immediately rather than waiting up to kHostResourcesInterval.
            next_host = now;
        }
        last_connected = is_connected;

        if (is_connected) {
            if (now >= next_host) {
                // Reuse the full HostResourcesPayload so resource ticks also
                // carry daemon_version / uptime / encoder_caps etc. The
                // browser merges payload over its host record on each tick,
                // so this keeps the Phase 5 host stats fresh without a
                // separate event type.
                auto payload = HostResourcesPayload();
                payload["updated_at"] = UnixNow();
                (void)SendJson({
                    {"id", GenerateUuid()},
                    {"type", "host.resources_tick"},
                    {"host_id", host_id_},
                    {"payload", std::move(payload)},
                });
                next_host = now + kHostResourcesInterval;
            }
            if (now >= next_vm) {
                auto vms = VmResourcesSnapshot();
                if (!vms.empty()) {
                    (void)SendJson({
                        {"id", GenerateUuid()},
                        {"type", "vm.resources_tick"},
                        {"host_id", host_id_},
                        {"payload", {
                            {"vms", std::move(vms)},
                            {"updated_at", UnixNow()},
                        }},
                    });
                }
                next_vm = now + kVmResourcesInterval;
            }
        } else {
            // WS down: advance schedule without sending so we do not burst
            // stale ticks when the socket reconnects.
            if (now >= next_host) next_host = now + kHostResourcesInterval;
            if (now >= next_vm) next_vm = now + kVmResourcesInterval;
        }

        // Drain any pending log batches every iteration. Live log followers
        // are sensitive to latency, so we flush at most every
        // kLogFlushIntervalMs (the per-iteration cap below).
        FlushLogBuffers();

        const auto wake = std::min(next_host, next_vm);
        auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wake - Clock::now()).count();
        // Cap per-iteration sleep at the log flush interval so log batches
        // never sit in memory longer than that without a push, and `running_`
        // is observed within the same window on shutdown.
        constexpr int64_t kMaxSleepMs = kLogFlushIntervalMs;
        if (wait_ms < 0) wait_ms = 0;
        if (wait_ms > kMaxSleepMs) wait_ms = kMaxSleepMs;
        if (wait_ms == 0) wait_ms = 50;
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
}

void CloudTunnel::EnqueueLogLines(const std::string& vm_id, const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    bool should_flush_now = false;
    {
        std::lock_guard<std::mutex> lock(log_buffer_mu_);
        auto& bucket = log_buffer_[vm_id];
        bucket.insert(bucket.end(), lines.begin(), lines.end());
        // Flush eagerly when a single VM has accumulated enough lines that
        // additional latency would noticeably hurt the follower experience.
        // Otherwise we wait for the tick thread to drain on its 200ms cadence.
        if (bucket.size() >= kLogBatchMaxLines) should_flush_now = true;
    }
    if (should_flush_now) FlushLogBuffers();
}

void CloudTunnel::FlushLogBuffers() {
    std::unordered_map<std::string, std::vector<std::string>> drained;
    {
        std::lock_guard<std::mutex> lock(log_buffer_mu_);
        if (log_buffer_.empty()) return;
        drained.swap(log_buffer_);
    }
    if (!connected_.load()) return;  // Drop silently when offline; tail RPC will catch up.
    for (auto& [vm_id, lines] : drained) {
        if (lines.empty()) continue;
        nlohmann::json arr = nlohmann::json::array();
        for (auto& line : lines) arr.push_back(std::move(line));
        (void)SendJson({
            {"type", "vm.logs.append"},
            {"host_id", host_id_},
            {"vm_id", vm_id},
            {"payload", {
                {"lines", std::move(arr)},
                {"emitted_at", UnixNow()},
            }},
        });
    }
}

nlohmann::json CloudTunnel::VmResourcesSnapshot() const {
    nlohmann::json vms = nlohmann::json::array();
    for (const auto& vm : store_.List()) {
        if (vm.runtime.state != VmState::kRunning || vm.runtime.pid <= 0) continue;
        vms.push_back({
            {"vm_id", vm.spec.vm_id},
            {"state", VmStateToString(vm.runtime.state)},
            {"pid", vm.runtime.pid},
            {"resources", {
                {"disk_usage_bytes", CachedDirectorySizeBytes(vm.spec.vm_dir)},
                {"process", ToJson(runtime_manager_.SampleProcessResources(vm.spec.vm_id))},
            }},
        });
    }
    return vms;
}

void CloudTunnel::PushVmStateChanged(const std::string& vm_id, const VmRuntimeInfo& info) {
    if (fd_ < 0) return;
    (void)SendJson({
        {"id", GenerateUuid()},
        {"type", "vm.state_changed"},
        {"host_id", host_id_},
        {"vm_id", vm_id},
        {"payload", {
            {"vm_id", vm_id},
            {"runtime", ToJson(info)},
        }},
    });
}

void CloudTunnel::PushImageCachedAdded(const std::string& cache_id, const std::string& image_name) {
    if (fd_ < 0) return;
    (void)SendJson({
        {"id", GenerateUuid()},
        {"type", "image.cached.added"},
        {"host_id", host_id_},
        {"payload", {
            {"cache_id", cache_id},
            {"image_name", image_name},
        }},
    });
}

void CloudTunnel::PushImageCachedRemoved(const std::string& cache_id) {
    if (fd_ < 0) return;
    (void)SendJson({
        {"id", GenerateUuid()},
        {"type", "image.cached.removed"},
        {"host_id", host_id_},
        {"payload", {{"cache_id", cache_id}}},
    });
}

void CloudTunnel::PushDownloadProgress(const DownloadJob& job) {
    if (fd_ < 0) return;
    (void)SendJson({
        {"id", GenerateUuid()},
        {"type", "image.download.progress"},
        {"host_id", host_id_},
        {"payload", DownloadJobToJson(job)},
    });
}

void CloudTunnel::PushDownloadTerminal(const DownloadJob& job) {
    if (fd_ < 0) return;
    const std::string type = job.status == "done"
        ? "image.download.completed"
        : (job.status == "cancelled" ? "image.download.cancelled" : "image.download.failed");
    (void)SendJson({
        {"id", GenerateUuid()},
        {"type", type},
        {"host_id", host_id_},
        {"payload", DownloadJobToJson(job)},
    });
}

void CloudTunnel::PublishRemoteAudio(const std::string& vm_id, RemoteAudioChunk chunk) {
    auto session = remote_sessions_.GetByVm(vm_id);
    if (!session) return;
    std::shared_ptr<WebRtcPeer> peer;
    {
        std::lock_guard<std::mutex> lock(remote_peers_mu_);
        auto it = remote_peers_.find(session->session_id);
        if (it != remote_peers_.end()) peer = it->second;
    }
    if (peer) peer->PushAudio(std::move(chunk));
}

nlohmann::json CloudTunnel::CreateVm(const nlohmann::json& payload) {
    const std::string cache_id = payload.value("image_cache_id", "");
    std::string kernel = payload.value("kernel", "");
    std::string initrd = payload.value("initrd", "");
    std::string disk = payload.value("disk", "");
    if (!cache_id.empty()) {
        const auto images_dir = ImagesDir(config_);
        for (const auto& image : image_source::GetCachedImages(images_dir)) {
            if (image.CacheId() != cache_id) continue;
            const auto cache_dir = image_source::ImageCacheDir(images_dir, image);
            kernel = FindImageFile(image, cache_dir, "kernel");
            initrd = FindImageFile(image, cache_dir, "initrd");
            disk = FindImageFile(image, cache_dir, "disk");
            break;
        }
    }

    VmSpec spec;
    spec.vm_id = GenerateUuid();
    spec.name = payload.value("name", spec.vm_id);
    spec.cmdline = "";
    spec.memory_mb = payload.value("memory_mb", static_cast<uint64_t>(4096));
    spec.cpu_count = payload.value("cpu_count", static_cast<uint32_t>(4));
    spec.nat_enabled = payload.value("net_enabled", true);
    spec.debug_mode = payload.value("debug_mode", false);
    spec.creation_time = UnixNow();
    spec.vm_dir = (std::filesystem::path(store_.VmRoot()) / spec.vm_id).string();

    std::error_code ec;
    std::filesystem::create_directories(spec.vm_dir, ec);
    if (ec) return Error("vm_create_failed", "failed to create VM directory: " + ec.message());

    auto copy = [](const std::string& source, const std::string& dir, std::string* out, std::string* error) {
        if (source.empty()) return true;
        std::filesystem::path src(source);
        if (!std::filesystem::exists(src)) {
            *error = "source file not found: " + source;
            return false;
        }
        auto dst = std::filesystem::path(dir) / src.filename();
        std::error_code ec;
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            *error = "failed to copy " + source + ": " + ec.message();
            return false;
        }
        *out = dst.string();
        return true;
    };

    std::string error;
    if (!copy(kernel, spec.vm_dir, &spec.kernel_path, &error) ||
        !copy(initrd, spec.vm_dir, &spec.initrd_path, &error) ||
        !copy(disk, spec.vm_dir, &spec.disk_path, &error)) {
        return Error("vm_create_failed", error);
    }
    if (spec.kernel_path.empty()) return Error("vm_create_failed", "kernel path is required");
    VmRecord created;
    if (!store_.Create(spec, &created, &error)) return Error("vm_create_failed", error);
    return ToJson(created);
}

nlohmann::json CloudTunnel::EditVm(const std::string& vm_id, const nlohmann::json& payload) {
    auto record = store_.Get(vm_id);
    if (!record) return Error("vm_not_found", "VM not found");
    VmSpec spec = record->spec;
    const bool running = record->runtime.state == VmState::kRunning ||
                         record->runtime.state == VmState::kStarting;

    if (payload.contains("name")) spec.name = payload.value("name", spec.name);
    if (payload.contains("memory_mb")) spec.memory_mb = payload.value("memory_mb", spec.memory_mb);
    if (payload.contains("cpu_count")) spec.cpu_count = payload.value("cpu_count", spec.cpu_count);
    if (payload.contains("net_enabled")) spec.nat_enabled = payload.value("net_enabled", spec.nat_enabled);
    const bool patch_net_enabled =
        payload.contains("net_enabled") && spec.nat_enabled != record->spec.nat_enabled;
    if (payload.contains("debug_mode")) spec.debug_mode = payload.value("debug_mode", spec.debug_mode);

    // Accept the new `host_forwards` key. We also accept the legacy
    // `port_forwards` key for older console payloads still in flight.
    const auto* host_forwards_payload =
        payload.contains("host_forwards") && payload["host_forwards"].is_array() ? &payload["host_forwards"]
        : payload.contains("port_forwards") && payload["port_forwards"].is_array() ? &payload["port_forwards"]
        : nullptr;
    const bool patch_host_forwards = host_forwards_payload != nullptr;
    if (patch_host_forwards) {
        spec.host_forwards.clear();
        for (const auto& item : *host_forwards_payload) {
            if (!item.is_object()) continue;
            HostForward pf;
            pf.host_port = item.value("host_port", 0);
            pf.guest_port = item.value("guest_port", 0);
            pf.host_ip = item.value("host_ip", "");
            pf.guest_ip = item.value("guest_ip", "");
            if (pf.host_port != 0 && pf.guest_port != 0) {
                spec.host_forwards.push_back(std::move(pf));
            }
        }
    }

    const bool patch_guest_forwards =
        payload.contains("guest_forwards") && payload["guest_forwards"].is_array();
    if (patch_guest_forwards) {
        spec.guest_forwards.clear();
        for (const auto& item : payload["guest_forwards"]) {
            if (!item.is_object()) continue;
            GuestForward gf;
            const std::string guest_ip_str = item.value("guest_ip", "");
            if (guest_ip_str.empty() ||
                !GuestForward::Ip4FromString(guest_ip_str, gf.guest_ip)) {
                continue;
            }
            gf.guest_port = item.value("guest_port", 0);
            gf.host_addr = item.value("host_addr", "");
            gf.host_port = item.value("host_port", 0);
            if (gf.guest_port != 0 && gf.host_port != 0) {
                spec.guest_forwards.push_back(std::move(gf));
            }
        }
    }

    const bool patch_shared_folders =
        payload.contains("shared_folders") && payload["shared_folders"].is_array();
    if (patch_shared_folders) {
        spec.shared_folders.clear();
        for (const auto& item : payload["shared_folders"]) {
            if (!item.is_object()) continue;
            SharedFolder sf;
            sf.tag = item.value("tag", "");
            sf.host_path = item.value("host_path", "");
            sf.readonly = item.value("readonly", false);
            if (!sf.tag.empty() && !sf.host_path.empty()) {
                spec.shared_folders.push_back(std::move(sf));
            }
        }
    }

    // Reject only when an offline-only field actually changed value: the
    // console always sends memory_mb / cpu_count / debug_mode in the payload
    // (even when the user is just editing shared folders), so a presence
    // check would lock out shared-folder / port-forward edits while running.
    if (running &&
        (spec.memory_mb != record->spec.memory_mb ||
         spec.cpu_count != record->spec.cpu_count ||
         spec.debug_mode != record->spec.debug_mode)) {
        return Error("vm_edit_requires_stopped", "memory/cpu/debug require the VM to be stopped");
    }

    std::string error;
    if (!store_.UpdateSpec(vm_id, spec, &error)) return Error("vm_edit_failed", error);

    // shared_folders / host_forwards / guest_forwards / net_enabled are
    // hot-updatable: the runtime applies virtiofs mounts, host/slirp port
    // listeners and the virtio-net link state in place. Failures here are
    // logged-but-not-fatal: the persisted spec is authoritative, so the
    // next start (or a guest reboot) picks up the change. ApplyForwards
    // covers both forward directions in one message; ApplyNetLink is
    // separate because it semantically toggles a different runtime piece
    // (the qemu netdev link state) and we want the link change to land
    // even when the forward set didn't change in the same edit.
    if (running && (patch_host_forwards || patch_guest_forwards)) {
        runtime_manager_.ApplyForwards(vm_id);
    }
    if (running && patch_shared_folders) runtime_manager_.ApplySharedFolders(vm_id);
    if (running && patch_net_enabled) runtime_manager_.ApplyNetLink(vm_id, spec.nat_enabled);

    auto updated = store_.Get(vm_id);
    return updated ? ToJson(*updated) : ToJson(VmRecord{.spec = spec});
}

nlohmann::json CloudTunnel::DeleteVm(const std::string& vm_id) {
    // Stop the runtime synchronously first. Otherwise the qemu/runtime
    // child keeps writing to `<vm_dir>/logs/runtime.log` and the daemon
    // keeps emitting `runtime_state.json` via VmStore::UpdateRuntime,
    // racing with the directory removal below: the rmdir succeeds but
    // a fresh `vm_dir` is recreated milliseconds later, leaving an
    // orphan directory the user perceives as "delete didn't work".
    if (auto record = store_.Get(vm_id); record &&
        (record->runtime.state == VmState::kRunning ||
         record->runtime.state == VmState::kStarting ||
         record->runtime.state == VmState::kStopping ||
         record->runtime.state == VmState::kRebooting)) {
        std::string stop_error;
        runtime_manager_.StopVm(vm_id, &stop_error);
        // StopVm only sends SIGTERM and returns; wait briefly until the
        // runtime exit path removes the session before we yank the dir.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            auto current = store_.Get(vm_id);
            if (!current || current->runtime.state == VmState::kStopped ||
                current->runtime.state == VmState::kCrashed) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    std::string vm_dir;
    if (auto record = store_.Get(vm_id)) {
        vm_dir = record->spec.vm_dir;
    }
    std::string error;
    if (!store_.Remove(vm_id, &error)) return Error("vm_delete_failed", error);
    EvictDiskCacheEntry(vm_dir);
    return nlohmann::json::object();
}

}  // namespace tenbox::daemon
