// T6 M0B injector CLI (game stream): mecvr_inject.exe.
//
//   mecvr_inject.exe --dll PATH --pid N
//   mecvr_inject.exe --dll PATH --wait NAME [--timeout SEC]
//   mecvr_inject.exe --shutdown PID
//   mecvr_inject.exe --find NAME
//
// --wait attaches to a Frosty-launched game by process name within a
// timeout (the demonstrated equivalent when FrostyModManager drives the
// launch). Exit codes: 0 ok, 1 usage, 2 failure.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "bootstrap/inject.h"

namespace {

void PrintUsage() {
  std::fputs(
      "usage:\n"
      "  mecvr_inject.exe --dll PATH --pid N\n"
      "  mecvr_inject.exe --dll PATH --wait NAME [--timeout SEC]\n"
      "  mecvr_inject.exe --launch-suspended EXE --cwd DIR --dll PATH [--dll PATH]\n"
      "  mecvr_inject.exe --shutdown PID\n"
      "  mecvr_inject.exe --find NAME\n",
      stdout);
}

bool ParseDword(const wchar_t* text, DWORD& out) {
  if (text == nullptr || *text == L'\0') return false;
  wchar_t* end = nullptr;
  const unsigned long v = ::wcstoul(text, &end, 10);
  if (end == text || *end != L'\0' || v == 0 || v > 0xFFFFFFFEul) return false;
  out = static_cast<DWORD>(v);
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::vector<std::wstring> dlls;
  DWORD pid = 0;
  std::wstring wait_name;
  std::wstring launch_path;
  std::wstring launch_cwd;
  int timeout_seconds = 120;
  bool do_shutdown = false;
  bool do_find = false;
  std::wstring find_name;

  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--dll" && i + 1 < argc) {
      dlls.emplace_back(argv[++i]);
    } else if (arg == L"--launch-suspended" && i + 1 < argc) {
      launch_path = argv[++i];
    } else if (arg == L"--cwd" && i + 1 < argc) {
      launch_cwd = argv[++i];
    } else if (arg == L"--pid" && i + 1 < argc) {
      if (!ParseDword(argv[++i], pid)) {
        std::fputs("invalid --pid value\n", stderr);
        return 1;
      }
    } else if (arg == L"--wait" && i + 1 < argc) {
      wait_name = argv[++i];
    } else if (arg == L"--timeout" && i + 1 < argc) {
      wchar_t* end = nullptr;
      const long v = ::wcstol(argv[++i], &end, 10);
      if (end == argv[i] || *end != L'\0' || v < 0 || v > 600) {
        std::fputs("invalid --timeout value (0..600)\n", stderr);
        return 1;
      }
      timeout_seconds = static_cast<int>(v);
    } else if (arg == L"--shutdown" && i + 1 < argc) {
      if (!ParseDword(argv[++i], pid)) {
        std::fputs("invalid --shutdown value\n", stderr);
        return 1;
      }
      do_shutdown = true;
    } else if (arg == L"--find" && i + 1 < argc) {
      do_find = true;
      find_name = argv[++i];
    } else if (arg == L"--help" || arg == L"-h") {
      PrintUsage();
      return 0;
    } else {
      std::fputws(L"unknown arg\n", stderr);
      PrintUsage();
      return 1;
    }
  }

  if (do_find) {
    const DWORD found =
        mecvr::bootstrap::FindPidByName(find_name);
    if (found == 0) {
      std::fputws(L"not found\n", stdout);
      return 2;
    }
    std::wprintf(L"found pid=%lu\n", static_cast<unsigned long>(found));
    return 0;
  }

  if (do_shutdown) {
    std::wstring error;
    if (!mecvr::bootstrap::SignalRuntimeShutdown(pid, error)) {
      std::fputws((L"shutdown signal failed: " + error + L"\n").c_str(),
                  stderr);
      return 2;
    }
    std::wprintf(L"shutdown signaled pid=%lu\n",
                 static_cast<unsigned long>(pid));
    return 0;
  }

  if (!launch_path.empty()) {
    if (pid != 0 || !wait_name.empty() || do_find || dlls.empty()) {
      std::fputs("--launch-suspended requires DLLs and excludes --pid/--wait/--find\n",
                 stderr);
      return 1;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + launch_path + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const wchar_t* cwd = launch_cwd.empty() ? nullptr : launch_cwd.c_str();
    if (!::CreateProcessW(launch_path.c_str(), mutable_command.data(), nullptr,
                          nullptr, FALSE, CREATE_SUSPENDED, nullptr, cwd,
                          &startup, &process)) {
      std::fwprintf(stderr, L"CreateProcessW suspended failed gle=%lu\n",
                    static_cast<unsigned long>(::GetLastError()));
      return 2;
    }
    bool injected = true;
    for (const auto& dll : dlls) {
      std::wstring error;
      if (!mecvr::bootstrap::InjectDllByPid(process.dwProcessId, dll, error)) {
        std::fputws((L"pre-resume inject failed: " + error + L"\n").c_str(),
                    stderr);
        injected = false;
        break;
      }
    }
    if (!injected || ::ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
      if (injected) {
        std::fwprintf(stderr, L"ResumeThread failed gle=%lu\n",
                      static_cast<unsigned long>(::GetLastError()));
      }
      // This process has never executed application code and cannot recover
      // from a partial preload. Terminate only this exact suspended child.
      ::TerminateProcess(process.hProcess, 2);
      ::CloseHandle(process.hThread);
      ::CloseHandle(process.hProcess);
      return 2;
    }
    std::wprintf(L"launched pid=%lu\n",
                 static_cast<unsigned long>(process.dwProcessId));
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return 0;
  }

  if (dlls.size() != 1) {
    std::fputs("missing --dll PATH\n", stderr);
    PrintUsage();
    return 1;
  }
  if (pid == 0 && wait_name.empty()) {
    std::fputs("need --pid N or --wait NAME\n", stderr);
    PrintUsage();
    return 1;
  }
  if (pid != 0 && !wait_name.empty()) {
    std::fputs("use only one of --pid / --wait\n", stderr);
    return 1;
  }

  DWORD target = pid;
  if (!wait_name.empty()) {
    std::wprintf(L"waiting for process...\n");
    target = mecvr::bootstrap::WaitForPidByName(wait_name, timeout_seconds);
    if (target == 0) {
      std::fputws(L"timed out waiting for process\n", stderr);
      return 2;
    }
    std::wprintf(L"attached pid=%lu\n", static_cast<unsigned long>(target));
  }

  std::wstring error;
  if (!mecvr::bootstrap::InjectDllByPid(target, dlls.front(), error)) {
    std::fputws((L"inject failed: " + error + L"\n").c_str(), stderr);
    return 2;
  }
  std::wprintf(L"injected pid=%lu\n", static_cast<unsigned long>(target));
  return 0;
}
