// T6 M0B injector implementation (game stream).
#include "bootstrap/inject.h"

#include <tlhelp32.h>

#include "bootstrap/runtime_worker.h"

namespace mecvr::bootstrap {
namespace {

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (::towlower(a[i]) != ::towlower(b[i])) return false;
  }
  return true;
}

std::wstring LastErrorText(DWORD gle) {
  wchar_t* msg = nullptr;
  const DWORD n = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, gle, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
  std::wstring out = L"gle=" + std::to_wstring(static_cast<unsigned long>(gle));
  if (n != 0 && msg != nullptr) {
    out += L" ";
    out += msg;
    while (!out.empty() &&
           (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ')) {
      out.pop_back();
    }
  }
  if (msg != nullptr) ::LocalFree(msg);
  return out;
}

}  // namespace

DWORD FindPidByName(const std::wstring& exe_name) {
  const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  DWORD found = 0;
  if (::Process32FirstW(snap, &pe)) {
    do {
      if (EqualsIgnoreCase(pe.szExeFile, exe_name)) {
        found = pe.th32ProcessID;
        break;
      }
    } while (::Process32NextW(snap, &pe));
  }
  ::CloseHandle(snap);
  return found;
}

DWORD WaitForPidByName(const std::wstring& exe_name, int timeout_seconds) {
  if (timeout_seconds < 0) timeout_seconds = 0;
  if (timeout_seconds > 600) timeout_seconds = 600;
  for (int waited = 0; waited <= timeout_seconds; ++waited) {
    const DWORD pid = FindPidByName(exe_name);
    if (pid != 0) return pid;
    if (waited < timeout_seconds) ::Sleep(1000);
  }
  return 0;
}

bool InjectDllByPid(DWORD pid, const std::wstring& dll_path,
                    std::wstring& error_out) {
  if (pid == 0 || dll_path.empty()) {
    error_out = L"invalid pid or empty dll path";
    return false;
  }
  // Resolve to an absolute path so the remote LoadLibraryW cannot depend
  // on the target's working directory.
  wchar_t abs_path[MAX_PATH] = {};
  const DWORD abs_n =
      ::GetFullPathNameW(dll_path.c_str(), static_cast<DWORD>(std::size(abs_path)),
                         abs_path, nullptr);
  if (abs_n == 0 || abs_n >= std::size(abs_path)) {
    error_out = L"cannot resolve dll path: " + LastErrorText(::GetLastError());
    return false;
  }

  const HANDLE proc = ::OpenProcess(
      PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
          PROCESS_VM_WRITE | PROCESS_VM_READ,
      FALSE, pid);
  if (proc == nullptr) {
    error_out = L"OpenProcess failed: " + LastErrorText(::GetLastError());
    return false;
  }

  bool ok = false;
  void* remote = nullptr;
  HANDLE thread = nullptr;
  const SIZE_T bytes =
      (static_cast<SIZE_T>(::wcslen(abs_path)) + 1) * sizeof(wchar_t);
  remote = ::VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
  if (remote == nullptr) {
    error_out =
        L"VirtualAllocEx failed: " + LastErrorText(::GetLastError());
  } else if (!::WriteProcessMemory(proc, remote, abs_path, bytes, nullptr)) {
    error_out =
        L"WriteProcessMemory failed: " + LastErrorText(::GetLastError());
  } else {
    const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) {
      error_out =
          L"GetModuleHandle(kernel32) failed: " + LastErrorText(::GetLastError());
    } else {
      FARPROC load_library = ::GetProcAddress(kernel32, "LoadLibraryW");
      if (load_library == nullptr) {
        error_out = L"GetProcAddress(LoadLibraryW) failed: " +
                    LastErrorText(::GetLastError());
      } else {
        thread = ::CreateRemoteThread(
            proc, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library), remote, 0,
            nullptr);
        if (thread == nullptr) {
          error_out = L"CreateRemoteThread failed: " +
                      LastErrorText(::GetLastError());
        } else {
          const DWORD wait =
              ::WaitForSingleObject(thread, kShutdownTimeoutMs * 6);
          if (wait != WAIT_OBJECT_0) {
            error_out = L"remote thread did not finish within budget";
          } else {
            DWORD remote_module = 0;
            if (!::GetExitCodeThread(thread, &remote_module)) {
              error_out = L"GetExitCodeThread failed: " +
                          LastErrorText(::GetLastError());
            } else if (remote_module == 0) {
              error_out = L"remote LoadLibraryW returned NULL";
            } else {
              ok = true;
            }
          }
        }
      }
    }
  }

  if (thread != nullptr) ::CloseHandle(thread);
  if (remote != nullptr) ::VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
  ::CloseHandle(proc);
  return ok;
}

bool SignalRuntimeShutdown(DWORD pid, std::wstring& error_out) {
  const std::wstring name = ShutdownEventName(pid);
  const HANDLE ev = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
  if (ev == nullptr) {
    error_out = L"OpenEvent failed: " + LastErrorText(::GetLastError());
    return false;
  }
  const BOOL set = ::SetEvent(ev);
  ::CloseHandle(ev);
  if (!set) {
    error_out = L"SetEvent failed: " + LastErrorText(::GetLastError());
    return false;
  }
  return true;
}

}  // namespace mecvr::bootstrap
