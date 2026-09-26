#pragma once

// T6 M0B injector API (game stream): classic CreateRemoteThread +
// LoadLibraryW injection by PID, plus a wait-for-process-name attach path
// for Frosty-launched games, plus an explicit-shutdown signal path.
//
// New file; owned by T6. Injection and lifecycle only — no hooks,
// no camera/input/render code (STOP S3).

#include <windows.h>

#include <string>

namespace mecvr::bootstrap {

// Finds a running process by exe basename (case-insensitive, e.g.
// L"MirrorsEdgeCatalyst.exe"). Returns 0 when absent. When several match,
// returns the first enumerated.
DWORD FindPidByName(const std::wstring& exe_name);

// Polls FindPidByName until found or timeout_seconds elapse.
// Returns 0 on timeout.
DWORD WaitForPidByName(const std::wstring& exe_name, int timeout_seconds);

// Injects an already-built DLL into pid via CreateRemoteThread+LoadLibraryW.
// dll_path must be an absolute path visible to the target. Returns true on
// success (remote LoadLibrary returned a module handle).
bool InjectDllByPid(DWORD pid, const std::wstring& dll_path,
                    std::wstring& error_out);

// Signals the runtime's named shutdown event for pid (the explicit
// shutdown path; the worker then exits within kShutdownTimeoutMs).
bool SignalRuntimeShutdown(DWORD pid, std::wstring& error_out);

}  // namespace mecvr::bootstrap
