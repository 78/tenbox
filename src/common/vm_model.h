#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct PortForward {
    uint16_t host_port;
    uint16_t guest_port;
    std::string host_ip;    // empty or "127.0.0.1" = loopback, "0.0.0.0" = LAN
    std::string guest_ip;   // empty = default guest IP (10.0.2.15)

    std::string EffectiveHostIp() const {
        return host_ip.empty() ? "127.0.0.1" : host_ip;
    }
    std::string EffectiveGuestIp() const {
        return guest_ip.empty() ? "" : guest_ip;
    }

    // "tcp:<host_bind>:<hport>-<guest_ip>:<gport>"
    std::string ToHostfwd() const {
        return std::string("tcp:") + EffectiveHostIp() + ":" +
               std::to_string(host_port) + "-" + EffectiveGuestIp() + ":" +
               std::to_string(guest_port);
    }

    // Parse hostfwd-style string. Returns true on success.
    // Accepts: "tcp:ADDR:HP-[GADDR]:GP" (full) or "HP:GP" / "HP:GP:L" (legacy)
    static bool FromHostfwd(const char* s, PortForward& out) {
        out = {};
        if (!s || !*s) return false;

        // Full format: "tcp:ADDR:HP-[GADDR]:GP"
        if (s[0] == 't' && s[1] == 'c' && s[2] == 'p' && s[3] == ':') {
            const char* p = s + 4;
            const char* colon1 = FindChar(p, ':');
            if (!colon1) return false;
            std::string bind_addr(p, colon1);
            p = colon1 + 1;
            const char* dash = FindChar(p, '-');
            if (!dash) return false;
            unsigned hp = ParseUint(p, dash);
            if (hp == 0 || hp > 65535) return false;
            p = dash + 1;
            // Parse optional guest address before :GP
            const char* last_colon = nullptr;
            for (const char* q = p; *q; ++q) {
                if (*q == ':') last_colon = q;
            }
            if (!last_colon) return false;
            std::string guest_addr(p, last_colon);
            p = last_colon + 1;
            const char* end = p;
            while (*end >= '0' && *end <= '9') ++end;
            unsigned gp = ParseUint(p, end);
            if (gp == 0 || gp > 65535) return false;
            out.host_port = static_cast<uint16_t>(hp);
            out.guest_port = static_cast<uint16_t>(gp);
            out.host_ip = bind_addr;
            out.guest_ip = guest_addr;
            return true;
        }

        // Legacy format: "HP:GP" or "HP:GP:LAN_FLAG"
        const char* p = s;
        const char* c1 = FindChar(p, ':');
        if (!c1) return false;
        unsigned hp = ParseUint(p, c1);
        if (hp == 0 || hp > 65535) return false;
        p = c1 + 1;
        const char* c2 = FindChar(p, ':');
        const char* gp_end = c2 ? c2 : FindEnd(p);
        unsigned gp = ParseUint(p, gp_end);
        if (gp == 0 || gp > 65535) return false;
        unsigned lan_flag = 0;
        if (c2) {
            p = c2 + 1;
            lan_flag = ParseUint(p, FindEnd(p));
        }
        out.host_port = static_cast<uint16_t>(hp);
        out.guest_port = static_cast<uint16_t>(gp);
        if (lan_flag) out.host_ip = "0.0.0.0";
        return true;
    }

private:
    static const char* FindChar(const char* s, char c) {
        for (; *s; ++s) if (*s == c) return s;
        return nullptr;
    }
    static const char* FindEnd(const char* s) {
        while (*s) ++s;
        return s;
    }
    static unsigned ParseUint(const char* begin, const char* end) {
        unsigned v = 0;
        for (const char* p = begin; p < end; ++p) {
            if (*p < '0' || *p > '9') return 0;
            unsigned next = v * 10 + static_cast<unsigned>(*p - '0');
            if (next < v) return 0;  // overflow
            v = next;
        }
        return (begin < end) ? v : 0;
    }
};

// Guest-initiated forwarding: guest connects to guest_ip:guest_port,
// traffic is forwarded to host_addr:host_port on the host.
struct GuestForward {
    uint32_t guest_ip = 0;        // network byte order not used; stored as host uint32
    uint16_t guest_port = 0;
    std::string host_addr;        // empty or "127.0.0.1" = loopback
    uint16_t host_port = 0;

    std::string EffectiveHostAddr() const {
        return host_addr.empty() ? "127.0.0.1" : host_addr;
    }

    // Format: "guestfwd:G_IP:GPORT-H_ADDR:HPORT"
    // host_addr may be omitted: "guestfwd:G_IP:GPORT-:HPORT" => 127.0.0.1
    std::string ToGuestfwd() const {
        return std::string("guestfwd:") + Ip4ToString(guest_ip) + ":" +
               std::to_string(guest_port) + "-" + EffectiveHostAddr() + ":" +
               std::to_string(host_port);
    }

    static bool FromGuestfwd(const char* s, GuestForward& out) {
        out = {};
        if (!s || !*s) return false;
        // Expect prefix "guestfwd:"
        const char* p = s;
        const char* prefix = "guestfwd:";
        for (int i = 0; prefix[i]; ++i) {
            if (*p != prefix[i]) return false;
            ++p;
        }
        // Parse guest IP (dotted quad until ':')
        const char* colon1 = FindChar(p, ':');
        if (!colon1) return false;
        uint32_t gip = 0;
        if (!ParseIp4(p, colon1, gip)) return false;
        p = colon1 + 1;
        // Parse guest port (digits until '-')
        const char* dash = FindChar(p, '-');
        if (!dash) return false;
        unsigned gport = ParseUint(p, dash);
        if (gport == 0 || gport > 65535) return false;
        p = dash + 1;
        // Parse host_addr:host_port — addr may be empty (e.g. "-:8080")
        const char* last_colon = FindLastChar(p, ':');
        if (!last_colon) return false;
        std::string haddr(p, last_colon);
        if (haddr.empty()) haddr = "127.0.0.1";
        p = last_colon + 1;
        const char* end = p;
        while (*end >= '0' && *end <= '9') ++end;
        unsigned hport = ParseUint(p, end);
        if (hport == 0 || hport > 65535) return false;

        out.guest_ip = gip;
        out.guest_port = static_cast<uint16_t>(gport);
        out.host_addr = haddr;
        out.host_port = static_cast<uint16_t>(hport);
        return true;
    }

    static bool Ip4FromString(const std::string& s, uint32_t& ip) {
        return ParseIp4(s.c_str(), s.c_str() + s.size(), ip);
    }
    static std::string Ip4ToString(uint32_t ip) {
        return std::to_string((ip >> 24) & 0xFF) + "." +
               std::to_string((ip >> 16) & 0xFF) + "." +
               std::to_string((ip >> 8) & 0xFF) + "." +
               std::to_string(ip & 0xFF);
    }

private:
    static const char* FindChar(const char* s, char c) {
        for (; *s; ++s) if (*s == c) return s;
        return nullptr;
    }
    static const char* FindLastChar(const char* s, char c) {
        const char* last = nullptr;
        for (; *s; ++s) if (*s == c) last = s;
        return last;
    }
    static unsigned ParseUint(const char* begin, const char* end) {
        unsigned v = 0;
        for (const char* p = begin; p < end; ++p) {
            if (*p < '0' || *p > '9') return 0;
            unsigned next = v * 10 + static_cast<unsigned>(*p - '0');
            if (next < v) return 0;
            v = next;
        }
        return (begin < end) ? v : 0;
    }
    static bool ParseIp4(const char* begin, const char* end, uint32_t& ip) {
        unsigned octets[4] = {};
        int idx = 0;
        const char* seg = begin;
        for (const char* p = begin; ; ++p) {
            if (p == end || *p == '.') {
                if (idx >= 4 || seg == p) return false;
                unsigned v = 0;
                for (const char* q = seg; q < p; ++q) {
                    if (*q < '0' || *q > '9') return false;
                    v = v * 10 + static_cast<unsigned>(*q - '0');
                }
                if (v > 255) return false;
                octets[idx++] = v;
                if (p == end) break;
                seg = p + 1;
            }
        }
        if (idx != 4) return false;
        ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
        return true;
    }
};

struct SharedFolder {
    std::string tag;        // virtiofs mount tag (e.g., "share")
    std::string host_path;  // host directory path
    bool readonly = false;
};

enum class VmPowerState : uint8_t {
    kStopped = 0,
    kStarting = 1,
    kRunning = 2,
    kStopping = 3,
    kCrashed = 4,
};

struct VmSpec {
    std::string name;
    std::string vm_id;       // UUID derived from directory name
    std::string vm_dir;      // absolute path to this VM's directory
    std::string kernel_path; // absolute at runtime, relative in vm.json
    std::string initrd_path;
    std::string disk_path;
    std::string cmdline;
    uint64_t memory_mb = 4096;
    uint32_t cpu_count = 4;
    bool nat_enabled = true;
    bool debug_mode = false;
    bool dpi_scaled = false;  // true = apply DPI scaling (lower VM res, larger text), false = 1:1 physical pixels
    std::vector<PortForward> port_forwards;
    std::vector<GuestForward> guest_forwards;
    std::vector<SharedFolder> shared_folders;
    int64_t creation_time = 0;   // Unix timestamp (seconds since epoch), 0 = not set
    int64_t last_boot_time = 0;  // Unix timestamp when VM was last started
};

struct VmMutablePatch {
    std::optional<std::string> name;
    std::optional<bool> debug_mode;
    std::optional<std::vector<PortForward>> port_forwards;
    std::optional<std::vector<GuestForward>> guest_forwards;
    std::optional<std::vector<SharedFolder>> shared_folders;
    std::optional<uint64_t> memory_mb;
    std::optional<uint32_t> cpu_count;
    bool apply_on_next_boot = false;
};
