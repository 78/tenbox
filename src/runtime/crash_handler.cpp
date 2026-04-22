#include "runtime/crash_handler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

namespace crash_handler {
namespace {

// ── Configuration captured at Install() ─────────────────────────────────────

struct Config {
    std::string crash_dir;   // absolute path to "<vm_dir>/crash"
    std::string vm_id;
    std::string version;
    bool installed = false;
};

Config g_config;
std::atomic<bool> g_handling{false};   // re-entrancy guard
std::atomic<bool> g_guest_panic_dumped{false};

// ── Guest console ring buffer ───────────────────────────────────────────────

// Roughly ~64 KiB of trailing guest console output is enough to carry a full
// kernel panic + stacktrace on a typical Linux image without making the host
// footprint heavy.
constexpr size_t kGuestRingSize = 64 * 1024;

struct GuestRing {
    std::mutex mu;
    char buf[kGuestRingSize]{};
    size_t head = 0;         // next write position
    size_t filled = 0;       // bytes currently valid (<= kGuestRingSize)

    // Rolling search state for panic markers. Kept tiny so we can walk the
    // live byte-stream without re-scanning the whole buffer on each Write().
    static constexpr size_t kTailLen = 64;
    char tail[kTailLen]{};   // last kTailLen bytes (logical, not ring-indexed)
    size_t tail_len = 0;
};

GuestRing g_guest;

// Panic marker substrings. We match against the rolling tail, so any of them
// appearing anywhere in the stream will trigger a guest_panic dump.
constexpr const char* kPanicNeedles[] = {
    "Kernel panic - not syncing",
    "end Kernel panic",
    "BUG: unable to handle",
    "Oops: ",
    "general protection fault:",
    "Call Trace:",
};

// ── Helpers ─────────────────────────────────────────────────────────────────

void EnsureCrashDir() {
    if (g_config.crash_dir.empty()) return;
#ifdef _WIN32
    CreateDirectoryA(g_config.crash_dir.c_str(), nullptr);
#else
    mkdir(g_config.crash_dir.c_str(), 0755);
#endif
}

// Compose a timestamped file basename like "20260422_081543_12345".
// Async-signal-safe on POSIX: no malloc, no printf.
void FormatStamp(char* out, size_t cap) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(out, cap, "%04u%02u%02u_%02u%02u%02u_%lu",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond,
                  static_cast<unsigned long>(GetCurrentProcessId()));
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm lt;
    time_t sec = tv.tv_sec;
    localtime_r(&sec, &lt);
    std::snprintf(out, cap, "%04d%02d%02d_%02d%02d%02d_%ld",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec,
                  static_cast<long>(getpid()));
#endif
}

std::string JoinPath(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif
    if (dir.back() == '/' || dir.back() == '\\') return dir + leaf;
    return dir + sep + leaf;
}

// Snapshot the guest ring into a contiguous string, oldest-first.
std::string SnapshotLocked(GuestRing& g) {
    std::string out;
    out.resize(g.filled);
    if (g.filled == kGuestRingSize) {
        // The ring has wrapped. head points to the oldest byte.
        size_t tail = kGuestRingSize - g.head;
        std::memcpy(out.data(), g.buf + g.head, tail);
        std::memcpy(out.data() + tail, g.buf, g.head);
    } else {
        std::memcpy(out.data(), g.buf, g.filled);
    }
    return out;
}

void WriteGuestPanicFile(const std::string& snapshot) {
    if (g_config.crash_dir.empty()) return;
    char stamp[64];
    FormatStamp(stamp, sizeof(stamp));
    std::string path = JoinPath(g_config.crash_dir,
                                std::string("guest_panic_") + stamp + ".txt");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "# TenBox guest panic capture\n");
    std::fprintf(f, "# vm_id:  %s\n", g_config.vm_id.c_str());
    std::fprintf(f, "# version:%s\n", g_config.version.c_str());
    std::fprintf(f, "# size:   %zu bytes\n\n", snapshot.size());
    std::fwrite(snapshot.data(), 1, snapshot.size(), f);
    std::fclose(f);
}

// Scan tail buffer for any configured panic marker.
bool TailMatchesPanic(const GuestRing& g) {
    if (g.tail_len == 0) return false;
    for (const char* needle : kPanicNeedles) {
        size_t nlen = std::strlen(needle);
        if (nlen > g.tail_len) continue;
        // Plain memmem-style search over the last tail_len bytes.
        for (size_t i = 0; i + nlen <= g.tail_len; ++i) {
            if (std::memcmp(g.tail + i, needle, nlen) == 0) return true;
        }
    }
    return false;
}

// ── Platform-specific crash dump ────────────────────────────────────────────

#ifdef _WIN32

void WriteMetaFile(const char* stamp, EXCEPTION_POINTERS* ep,
                   const std::string& guest_tail) {
    std::string path = JoinPath(g_config.crash_dir,
                                std::string("crash_") + stamp + ".meta");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;

    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;

    std::fprintf(f, "TenBox runtime crash\n");
    std::fprintf(f, "version : %s\n", g_config.version.c_str());
    std::fprintf(f, "vm_id   : %s\n", g_config.vm_id.c_str());
    std::fprintf(f, "pid     : %lu\n", GetCurrentProcessId());
    std::fprintf(f, "tid     : %lu\n", GetCurrentThreadId());
    std::fprintf(f, "code    : 0x%08lx\n", code);
    std::fprintf(f, "address : 0x%p\n", addr);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep && ep->ExceptionRecord
        && ep->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR* p = ep->ExceptionRecord->ExceptionInformation;
        const char* kind = p[0] == 0 ? "read" : (p[0] == 1 ? "write" : "exec");
        std::fprintf(f, "av_kind : %s\n", kind);
        std::fprintf(f, "av_addr : 0x%p\n", reinterpret_cast<void*>(p[1]));
    }
    std::fprintf(f, "\n-- last guest console (%zu bytes) --\n", guest_tail.size());
    std::fwrite(guest_tail.data(), 1, guest_tail.size(), f);
    std::fclose(f);
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* ep) {
    bool expected = false;
    if (!g_handling.compare_exchange_strong(expected, true)) {
        // Already handling a prior crash on another thread: just terminate.
        return EXCEPTION_EXECUTE_HANDLER;
    }

    EnsureCrashDir();
    char stamp[64];
    FormatStamp(stamp, sizeof(stamp));

    // Snapshot guest console first (cheap, no external calls).
    std::string guest_tail;
    {
        // Best-effort: if the lock is currently held by the faulting thread
        // try_lock prevents a deadlock at the cost of missing a few bytes.
        if (g_guest.mu.try_lock()) {
            guest_tail = SnapshotLocked(g_guest);
            g_guest.mu.unlock();
        }
    }

    if (!g_config.crash_dir.empty()) {
        std::string dump_path = JoinPath(g_config.crash_dir,
                                        std::string("crash_") + stamp + ".dmp");

        HANDLE file = CreateFileA(dump_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{};
            info.ThreadId = GetCurrentThreadId();
            info.ExceptionPointers = ep;
            info.ClientPointers = FALSE;

            // MiniDumpWithDataSegs + indirectly referenced memory gives a
            // useful stack-walkable dump without bloating to a full heap.
            // Keep the size manageable — host VM process may have many GB of
            // guest RAM mapped, which we deliberately omit.
            MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithDataSegs |
                MiniDumpWithHandleData |
                MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpWithThreadInfo |
                MiniDumpWithUnloadedModules);

            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                               file, type, ep ? &info : nullptr, nullptr, nullptr);
            FlushFileBuffers(file);
            CloseHandle(file);
        }

        WriteMetaFile(stamp, ep, guest_tail);

        // Also write a line to stderr so it shows up in runtime.log.
        std::fprintf(stderr, "\n[FATAL] tenbox-vm-runtime crashed: code=0x%08lx "
                             "dump=%s\n",
                     ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0ul,
                     dump_path.c_str());
        std::fflush(stderr);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

// Some CRT failures bypass the unhandled exception filter; route them back in.
void OnPureCall() { RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr); }
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                        unsigned int, uintptr_t) {
    RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
void OnTerminate() { RaiseException(0xE0000003, EXCEPTION_NONCONTINUABLE, 0, nullptr); }

void InstallPlatform() {
    SetUnhandledExceptionFilter(&TopLevelFilter);
    _set_purecall_handler(&OnPureCall);
    _set_invalid_parameter_handler(&OnInvalidParameter);
    std::set_terminate(&OnTerminate);

    // Opt out of the Windows Error Reporting "crashed app" popup dialog so
    // the process exits immediately after our filter returns.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

#else  // POSIX

// Alternate signal stack so crash handlers still run when the fault is stack
// overflow (normal stack is invalid). SA_ONSTACK is only set after sigaltstack
// succeeds; otherwise the kernel ignores SA_ONSTACK on Linux.
#if defined(SIGSTKSZ) && (SIGSTKSZ > 0)
constexpr size_t kAltStackBytes = SIGSTKSZ + 128 * 1024;
#else
constexpr size_t kAltStackBytes = 128 * 1024;
#endif
alignas(4096) static unsigned char g_alt_stack[kAltStackBytes];

void WriteMetaFilePosix(const char* stamp, int signo, const void* addr,
                        const std::string& guest_tail) {
    std::string path = JoinPath(g_config.crash_dir,
                                std::string("crash_") + stamp + ".meta");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "TenBox runtime crash\n");
    std::fprintf(f, "version : %s\n", g_config.version.c_str());
    std::fprintf(f, "vm_id   : %s\n", g_config.vm_id.c_str());
    std::fprintf(f, "pid     : %ld\n", static_cast<long>(getpid()));
    std::fprintf(f, "signal  : %d (%s)\n", signo, strsignal(signo));
    std::fprintf(f, "address : %p\n", addr);
    std::fprintf(f, "\n-- last guest console (%zu bytes) --\n", guest_tail.size());
    std::fwrite(guest_tail.data(), 1, guest_tail.size(), f);
    std::fclose(f);
}

void SignalHandler(int signo, siginfo_t* info, void* /*ucontext*/) {
    bool expected = false;
    if (!g_handling.compare_exchange_strong(expected, true)) {
        // Already handling — reset to default and re-raise so the kernel can
        // produce a real core file.
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigaction(signo, &sa, nullptr);
        raise(signo);
        return;
    }

    EnsureCrashDir();

    char stamp[64];
    FormatStamp(stamp, sizeof(stamp));

    std::string guest_tail;
    if (g_guest.mu.try_lock()) {
        guest_tail = SnapshotLocked(g_guest);
        g_guest.mu.unlock();
    }

    // backtrace() is not strictly async-signal-safe on all libc versions, but
    // in practice it is safe enough for SIGSEGV/SIGABRT post-mortem. A crashed
    // process that fails to walk its stack is not worse than no trace at all.
    std::string trace_path = JoinPath(g_config.crash_dir,
                                      std::string("crash_") + stamp + ".trace");
    int fd = ::open(trace_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        void* frames[128];
        int n = backtrace(frames, 128);
        dprintf(fd, "TenBox runtime crash\n");
        dprintf(fd, "version : %s\n", g_config.version.c_str());
        dprintf(fd, "vm_id   : %s\n", g_config.vm_id.c_str());
        dprintf(fd, "pid     : %ld\n", static_cast<long>(getpid()));
        dprintf(fd, "signal  : %d\n", signo);
        if (info) dprintf(fd, "address : %p\n", info->si_addr);
        dprintf(fd, "frames  : %d\n\n", n);
        backtrace_symbols_fd(frames, n, fd);
        ::close(fd);
    }

    WriteMetaFilePosix(stamp, signo, info ? info->si_addr : nullptr, guest_tail);

    std::fprintf(stderr,
                 "\n[FATAL] tenbox-vm-runtime crashed: signal=%d trace=%s\n",
                 signo, trace_path.c_str());
    std::fflush(stderr);

    // Re-raise with the default disposition so shells see the right exit
    // status and the kernel can still write a real core dump if configured.
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigaction(signo, &sa, nullptr);
    raise(signo);
}

void InstallPlatform() {
    stack_t ss{};
    ss.ss_sp = g_alt_stack;
    ss.ss_size = sizeof(g_alt_stack);
    ss.ss_flags = 0;
    const bool alt_ok = (sigaltstack(&ss, nullptr) == 0);

    struct sigaction sa{};
    sa.sa_sigaction = &SignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    if (alt_ok) {
        sa.sa_flags |= SA_ONSTACK;
    }
    sigemptyset(&sa.sa_mask);

    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        sigaction(s, &sa, nullptr);
    }
}

#endif  // _WIN32

}  // namespace

void Install(const std::string& vm_dir,
             const std::string& vm_id,
             const std::string& build_version) {
    // Config fields can be refined by later Install() calls (e.g. once the
    // --vm-dir argument has been parsed), but the platform-level hooks are
    // installed only once.
    if (!vm_id.empty()) g_config.vm_id = vm_id;
    if (!build_version.empty()) g_config.version = build_version;
    if (!vm_dir.empty()) {
        g_config.crash_dir = JoinPath(vm_dir, "crash");
        EnsureCrashDir();
    }
    if (g_config.installed) return;
    g_config.installed = true;
    InstallPlatform();
}

void RecordGuestConsole(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;

    bool panic_hit = false;
    std::string snap;
    {
        std::lock_guard<std::mutex> lock(g_guest.mu);

        // Append into the ring buffer.
        for (size_t i = 0; i < size; ++i) {
            g_guest.buf[g_guest.head] = static_cast<char>(data[i]);
            g_guest.head = (g_guest.head + 1) % kGuestRingSize;
            if (g_guest.filled < kGuestRingSize) ++g_guest.filled;
        }

        // Update rolling tail used for panic-marker detection.
        size_t take = std::min(size, GuestRing::kTailLen);
        if (take < size) {
            std::memcpy(g_guest.tail, data + size - take, take);
            g_guest.tail_len = take;
        } else {
            if (g_guest.tail_len + take > GuestRing::kTailLen) {
                size_t drop = g_guest.tail_len + take - GuestRing::kTailLen;
                std::memmove(g_guest.tail, g_guest.tail + drop,
                             g_guest.tail_len - drop);
                g_guest.tail_len -= drop;
            }
            std::memcpy(g_guest.tail + g_guest.tail_len, data, take);
            g_guest.tail_len += take;
        }

        if (!g_guest_panic_dumped.load(std::memory_order_relaxed)
            && TailMatchesPanic(g_guest)) {
            bool expected = false;
            if (g_guest_panic_dumped.compare_exchange_strong(expected, true)) {
                snap = SnapshotLocked(g_guest);
                panic_hit = true;
            }
        }
    }

    if (panic_hit) {
        // File IO outside the ring-buffer lock so the guest console path
        // stays unblocked while we flush to disk.
        WriteGuestPanicFile(snap);
        std::fprintf(stderr, "[FATAL] Guest kernel panic detected, console "
                             "snapshot written under %s\n",
                     g_config.crash_dir.c_str());
        std::fflush(stderr);
    }
}

std::string SnapshotGuestConsole() {
    std::lock_guard<std::mutex> lock(g_guest.mu);
    return SnapshotLocked(g_guest);
}

}  // namespace crash_handler
