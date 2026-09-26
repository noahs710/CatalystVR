// T6 M0B safe runtime DLL (game stream): mecvr_runtime.dll.
//
// Lifecycle contract:
//   * DLL_PROCESS_ATTACH starts exactly one bounded worker thread.
//   * DLL_PROCESS_DETACH (or an explicit shutdown signal) stops it within
//     kShutdownTimeoutMs (bounded shutdown; hard gate).
//   * NO FreeLibrary hot-unload path is offered (the plan forbids
//     perfecting it now); unload is process exit.
//   * ZERO graphics hooks, ZERO camera/input/render code (STOP S3).
//
// The worker opens the file logger, emits a periodic heartbeat line, and
// hosts a minimal IPC skeleton for future diagnostics polling: a named
// shutdown event (explicit-shutdown path), a named-pipe name reserved +
// created (unconnected; future polling plugs in here), and a shared-memory
// status block updated every heartbeat.

#include "bootstrap/runtime_worker.h"

#include <process.h>
#include <windows.h>

#include <string>

#include "diagnostics/logging.h"

namespace mecvr::bootstrap {

std::wstring RuntimeLogPath() {
  wchar_t temp[MAX_PATH] = {};
  DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
  std::wstring path;
  if (n == 0 || n >= std::size(temp)) {
    path = L".\\";
  } else {
    path = temp;
  }
  path += L"mecvr_runtime.log";
  return path;
}

}  // namespace mecvr::bootstrap

namespace {

using mecvr::bootstrap::RuntimeStatus;

volatile LONG g_started = 0;  // 0 = no worker, 1 = worker owned.
HANDLE g_shutdown_event = nullptr;  // unnamed, manual-reset; set on detach.
HANDLE g_worker_thread = nullptr;
HANDLE g_named_shutdown = nullptr;  // explicit-shutdown path for tooling.
HANDLE g_pipe = INVALID_HANDLE_VALUE;
HANDLE g_status_map = nullptr;
RuntimeStatus* g_status = nullptr;
volatile LONG g_tick = 0;  // latest heartbeat tick (exported for polling).

std::string ToNarrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                      nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n) - 1, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr,
                        nullptr);
  return out;
}

void PublishStatus(bool running, LONG tick, DWORD pid) {
  if (g_status != nullptr) {
    g_status->pid = pid;
    g_status->tick = tick;
    ::MemoryBarrier();
    g_status->running = running ? 1 : 0;
  }
}

// _beginthreadex (not CreateThread): the worker uses the CRT (logger,
// strings), so it needs CRT thread init. Returns unsigned __stdcall.
unsigned __stdcall WorkerThreadProc(void* /*param*/) {
  using mecvr::diagnostics::Level;
  using mecvr::diagnostics::Logger;

  const DWORD pid = ::GetCurrentProcessId();
  const std::wstring log_path = mecvr::bootstrap::RuntimeLogPath();
  Logger::instance().open(ToNarrow(log_path));

  const std::wstring event_name = mecvr::bootstrap::ShutdownEventName(pid);
  g_named_shutdown =
      ::CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());

  // IPC skeleton: reserve + create the pipe now (left unconnected; future
  // diagnostics polling connects here), and publish a shared-memory
  // status block updated on every heartbeat.
  const std::string pipe_name = mecvr::bootstrap::PipeNameA(pid);
  g_pipe = ::CreateNamedPipeA(
      pipe_name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0,
      nullptr);

  const std::wstring map_name = mecvr::bootstrap::StatusMapName(pid);
  g_status_map = ::CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(RuntimeStatus),
      map_name.c_str());
  if (g_status_map != nullptr) {
    g_status = static_cast<RuntimeStatus*>(::MapViewOfFile(
        g_status_map, FILE_MAP_WRITE, 0, 0, sizeof(RuntimeStatus)));
  }

  Logger::instance().log(
      Level::kInfo, "worker-started pid=" + std::to_string(pid) +
                        " log=" + ToNarrow(log_path));

  LONG tick = 0;
  HANDLE wait_handles[2] = {g_shutdown_event, g_named_shutdown};
  const DWORD wait_count =
      (g_named_shutdown != nullptr) ? 2u : 1u;
  for (;;) {
    const DWORD w =
        ::WaitForMultipleObjects(wait_count, wait_handles, FALSE,
                                 mecvr::bootstrap::kHeartbeatMs);
    if (w == WAIT_FAILED || w == WAIT_OBJECT_0 || w == WAIT_OBJECT_0 + 1) {
      break;  // detach signal, explicit shutdown signal, or wait failure.
    }
    // WAIT_TIMEOUT: emit heartbeat.
    ++tick;
    ::InterlockedExchange(&g_tick, tick);
    PublishStatus(true, tick, pid);
    Logger::instance().log(
        Level::kInfo, "heartbeat tick=" + std::to_string(tick) +
                          " pid=" + std::to_string(pid));
  }

  PublishStatus(false, tick, pid);
  Logger::instance().log(Level::kInfo, "worker-stopped pid=" +
                                           std::to_string(pid) + " tick=" +
                                           std::to_string(tick));

  if (g_status != nullptr) {
    ::UnmapViewOfFile(g_status);
    g_status = nullptr;
  }
  if (g_status_map != nullptr) {
    ::CloseHandle(g_status_map);
    g_status_map = nullptr;
  }
  if (g_pipe != INVALID_HANDLE_VALUE) {
    ::CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
  }
  if (g_named_shutdown != nullptr) {
    ::CloseHandle(g_named_shutdown);
    g_named_shutdown = nullptr;
  }
  Logger::instance().close();
  return 0;
}

void StartWorkerOnce() {
  if (::InterlockedCompareExchange(&g_started, 1, 0) != 0) {
    return;  // exactly one worker: a second attach never starts another.
  }
  g_shutdown_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_shutdown_event == nullptr) {
    ::InterlockedExchange(&g_started, 0);
    return;
  }
  unsigned thread_id = 0;
  const uintptr_t th = ::_beginthreadex(nullptr, 0, &WorkerThreadProc,
                                        nullptr, 0, &thread_id);
  if (th == 0) {
    ::CloseHandle(g_shutdown_event);
    g_shutdown_event = nullptr;
    ::InterlockedExchange(&g_started, 0);
    return;
  }
  g_worker_thread = reinterpret_cast<HANDLE>(th);
}

void StopWorkerBounded() {
  if (::InterlockedCompareExchange(&g_started, 0, 1) != 1) {
    return;  // no worker owned; nothing to stop.
  }
  if (g_shutdown_event != nullptr) {
    ::SetEvent(g_shutdown_event);
  }
  if (g_worker_thread != nullptr) {
    // Bounded shutdown: fixed timeout, never infinite. No FreeLibrary
    // hot-unload path exists by design.
    ::WaitForSingleObject(g_worker_thread,
                          mecvr::bootstrap::kShutdownTimeoutMs);
    ::CloseHandle(g_worker_thread);
    g_worker_thread = nullptr;
  }
  if (g_shutdown_event != nullptr) {
    ::CloseHandle(g_shutdown_event);
    g_shutdown_event = nullptr;
  }
}

}  // namespace

extern "C" {

// Explicit-shutdown path for tooling (the injector CLI signals the named
// event; this export covers in-process callers). Never unloads the DLL.
__declspec(dllexport) void MecvrRequestShutdown(void) {
  if (g_shutdown_event != nullptr) {
    ::SetEvent(g_shutdown_event);
  }
}

// Latest heartbeat tick for diagnostics polling (0 before first heartbeat).
__declspec(dllexport) long MecvrHeartbeatTick(void) {
  return static_cast<long>(g_tick);
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      ::DisableThreadLibraryCalls(module);
      StartWorkerOnce();
      break;
    case DLL_PROCESS_DETACH:
      StopWorkerBounded();
      break;
    default:
      break;
  }
  return TRUE;
}
