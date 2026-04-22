#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Process-wide crash dump support.
//
// When installed, fatal errors in the host runtime (SEH access violations on
// Windows, SIGSEGV / SIGABRT / etc. on POSIX) are intercepted and a
// post-mortem bundle is written under "<vm_dir>/crash/":
//
//   crash_<timestamp>.dmp   - Windows minidump (with heap & handle data)
//                             or a "<timestamp>.trace" backtrace on POSIX
//   crash_<timestamp>.meta  - crash metadata: pid, reason, faulting address,
//                             build info, last guest console bytes
//
// The guest console ring-buffer is also dumped whenever a guest kernel panic
// or Oops is detected on the serial stream (see RecordGuestConsole).
namespace crash_handler {

// Initialize the crash handler. Must be called once, as early in main() as
// possible (before any allocations in worker threads). Safe to call with an
// empty vm_dir; in that case dumps fall back to the process cwd.
// vm_id is recorded in the metadata and helps identify dumps across VMs.
void Install(const std::string& vm_dir,
             const std::string& vm_id,
             const std::string& build_version);

// Feed raw guest console bytes into a bounded ring-buffer and scan them
// for well-known Linux panic markers. Thread-safe. When a panic marker is
// detected a guest_panic_<timestamp>.txt file is written under the crash
// directory (only the first occurrence per process is recorded).
void RecordGuestConsole(const uint8_t* data, size_t size);

// Snapshot the last N bytes of the guest console ring-buffer. Used by the
// host crash path to embed guest context into the metadata file.
std::string SnapshotGuestConsole();

}  // namespace crash_handler
