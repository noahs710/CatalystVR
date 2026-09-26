#pragma once

// T6 M0B safe runtime worker contract (game stream).
//
// Injection and lifecycle ONLY. This header names the log location, the
// fixed timing bounds, and the future-diagnostics IPC skeleton object
// names. It intentionally contains ZERO graphics, camera, input, or
// render declarations (STOP S3).
//
// Owned by T6; new file (nothing here existed before).

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace mecvr::bootstrap {

// Log file lives in %TEMP% (never the game directory, which is read-only).
std::wstring RuntimeLogPath();

// Per-process object names so parallel game instances cannot collide.
inline std::wstring ShutdownEventName(DWORD pid) {
  wchar_t buf[64] = {};
  ::swprintf_s(buf, L"Local\\MECVR_RUNTIME_SHUTDOWN_%lu",
               static_cast<unsigned long>(pid));
  return std::wstring(buf);
}

inline std::string PipeNameA(DWORD pid) {
  char buf[64] = {};
  ::sprintf_s(buf, "\\\\.\\pipe\\mecvr_diag_%lu",
              static_cast<unsigned long>(pid));
  return std::string(buf);
}

inline std::wstring StatusMapName(DWORD pid) {
  wchar_t buf[64] = {};
  ::swprintf_s(buf, L"Local\\mecvr_status_%lu",
               static_cast<unsigned long>(pid));
  return std::wstring(buf);
}

// Fixed bounds (hard gate: bounded-shutdown-only).
inline constexpr DWORD kHeartbeatMs = 2000;
inline constexpr DWORD kShutdownTimeoutMs = 5000;

// Shared-memory status block: written by the worker once per heartbeat,
// read by future diagnostics polling. Plain POD, no pointers.
struct RuntimeStatus {
  volatile LONG running;  // 1 while the worker loop is alive, else 0.
  volatile LONG tick;     // heartbeat counter.
  DWORD pid;              // owning game process id.
};

}  // namespace mecvr::bootstrap
