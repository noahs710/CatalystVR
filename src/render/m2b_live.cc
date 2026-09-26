// M2B live mono submitter (render stream, T11-live). Builds m2b_live.dll,
// injected into the running game with the T6 injector.
//
// WHAT IT DOES (M2 scope only): passively observes Present on the shared
// swapchain vtables (T3B technique), copies the proven final-output
// backbuffer into immutable MonoFrames, publishes through the bounded
// mailbox, and submits the IDENTICAL mono image to both eyes through the
// real OpenXR backend on its dedicated XR worker thread.
//
// WHAT IT NEVER DOES (STOP S3): no camera reads/writes, no stereo offsets,
// no simulation/timing/frame-limiter hooks, no gameplay input, no HUD
// work, no game-directory writes. Present-thread work is bounded (a
// throttled staging copy); it never calls xrWaitFrame or waits on XR.
//
// Throttle: a capture is taken only when the mailbox has no unconsumed
// frame AND >=4 ms elapsed since the last capture. Newest-wins makes
// extra captures pure overhead, so this protects the ~200 fps game
// cadence while keeping capture-to-submit age low.

#include <windows.h>

#include <dxgi1_5.h>

#include <d3d11.h>

#include <process.h>
#include <strsafe.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "openxr/mailbox.h"
#include "openxr/real_xr_backend.h"
#include "openxr/xr_frame_worker.h"
#include "render/m2b_mono.h"
#include "render/observer.h"  // StateGuard (game-context preservation).

constexpr std::size_t kPresentIndex = 8;
constexpr std::size_t kResizeBuffersIndex = 13;

// Named (not anonymous) namespace: these symbols must survive optimization
// with external linkage. An anonymous namespace let /O2 prove the whole TU
// unreachable (internal DllMain) and emit an empty object — silent, no
// warning. Never use an anonymous namespace in an injected-module TU.
namespace m2b_live {

constexpr int kMaxVtables = 8;
constexpr DWORD kWorkerWaitMs = 2000;
constexpr std::int64_t kMinCaptureIntervalNs = 4000000;  // 4 ms throttle.

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

struct VtableEntry {
  void** vtable = nullptr;
  void* orig_present = nullptr;
  void* orig_resize = nullptr;
};

VtableEntry g_vtables[kMaxVtables];
volatile LONG g_vtable_count = 0;

mecvr::openxr::FrameMailbox* g_mailbox = nullptr;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

HANDLE g_worker_thread = nullptr;
HANDLE g_own_shutdown = nullptr;
volatile LONG g_detaching = 0;
DWORD g_pid = 0;
HANDLE g_log = INVALID_HANDLE_VALUE;
volatile LONGLONG g_presents = 0;
volatile LONGLONG g_captures = 0;
volatile LONGLONG g_state_fail = 0;
volatile LONGLONG g_capture_skipped_throttle = 0;
std::int64_t g_last_capture_ns = 0;

// Game-device staging copy (owned by the Present thread only).
ID3D11Texture2D* g_staging = nullptr;
UINT g_staging_w = 0;
UINT g_staging_h = 0;
DXGI_FORMAT g_staging_fmt = DXGI_FORMAT_UNKNOWN;

std::int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void LogLine(const char* text) {
  if (g_log == INVALID_HANDLE_VALUE || text == nullptr) return;
  size_t len = 0;
  if (SUCCEEDED(StringCchLengthA(text, 2000, &len))) {
    DWORD written = 0;
    WriteFile(g_log, text, static_cast<DWORD>(len), &written, nullptr);
    WriteFile(g_log, "\r\n", 2, &written, nullptr);
  }
}

void LogF(const char* fmt, ...) {
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  if (SUCCEEDED(StringCchVPrintfA(buf, _countof(buf), fmt, args))) LogLine(buf);
  va_end(args);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool PatchSlot(void** vtable, std::size_t index, void* detour,
               void** original_out) {
  DWORD old_protect = 0;
  if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE,
                     &old_protect) == 0) {
    return false;
  }
  *original_out = vtable[index];
  vtable[index] = detour;
  DWORD ignored = 0;
  VirtualProtect(&vtable[index], sizeof(void*), old_protect, &ignored);
  return true;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync,
                                      UINT flags);
HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT count,
                                            UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags);

bool HookVtable(IDXGISwapChain* chain) {
  void** vtable = *reinterpret_cast<void***>(chain);
  for (LONG i = 0; i < g_vtable_count; ++i) {
    if (g_vtables[i].vtable == vtable) return true;
  }
  if (g_vtable_count >= kMaxVtables) return false;
  VtableEntry& e = g_vtables[g_vtable_count];
  e.vtable = vtable;
  e.orig_present = vtable[kPresentIndex];
  e.orig_resize = vtable[kResizeBuffersIndex];
  MemoryBarrier();
  InterlockedExchange(&g_vtable_count, g_vtable_count + 1);
  void* check_p = nullptr;
  void* check_r = nullptr;
  return PatchSlot(vtable, kPresentIndex, reinterpret_cast<void*>(&HookPresent),
                   &check_p) &&
         PatchSlot(vtable, kResizeBuffersIndex,
                   reinterpret_cast<void*>(&HookResizeBuffers), &check_r);
}

// Capture the backbuffer into an immutable MonoFrame and publish it.
// Runs on the game render thread; strictly bounded (throttled above),
// never waits on XR. Returns without publishing when throttled or on
// any failure (missed capture, never a stall).
void CaptureAndPublish(IDXGISwapChain* self) {
  const std::int64_t now = SteadyNs();
  if (now - g_last_capture_ns < kMinCaptureIntervalNs) {
    InterlockedIncrement64(&g_capture_skipped_throttle);
    return;
  }
  if (g_mailbox != nullptr && g_mailbox->depth() > 0) {
    InterlockedIncrement64(&g_capture_skipped_throttle);
    return;  // An unconsumed frame is already queued; newest-wins covers it.
  }
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11Texture2D* back = nullptr;
  bool published = false;
  do {
    if (FAILED(self->GetDevice(__uuidof(ID3D11Device),
                               reinterpret_cast<void**>(&device))) ||
        device == nullptr) {
      break;
    }
    device->GetImmediateContext(&context);
    if (context == nullptr) break;
    if (FAILED(self->GetBuffer(0, __uuidof(ID3D11Texture2D),
                               reinterpret_cast<void**>(&back))) ||
        back == nullptr) {
      break;
    }
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    if (bd.Width == 0 || bd.Height == 0 || bd.Width > 8192 ||
        bd.Height > 8192) {
      break;
    }
    if (g_staging == nullptr || g_staging_w != bd.Width ||
        g_staging_h != bd.Height || g_staging_fmt != bd.Format) {
      if (g_staging != nullptr) {
        g_staging->Release();
        g_staging = nullptr;
      }
      D3D11_TEXTURE2D_DESC sd{};
      sd.Width = bd.Width;
      sd.Height = bd.Height;
      sd.MipLevels = 1;
      sd.ArraySize = 1;
      sd.Format = bd.Format;
      sd.SampleDesc.Count = 1;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(device->CreateTexture2D(&sd, nullptr, &g_staging)) ||
          g_staging == nullptr) {
        break;
      }
      g_staging_w = bd.Width;
      g_staging_h = bd.Height;
      g_staging_fmt = bd.Format;
    }
    context->CopyResource(g_staging, back);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_staging, 0, D3D11_MAP_READ, 0, &mapped))) break;
    auto frame = std::make_shared<mecvr::render::MonoFrame>();
    frame->sequence =
        static_cast<std::uint64_t>(InterlockedIncrement64(&g_presents));
    frame->capture_time_ns = SteadyNs();
    frame->width = bd.Width;
    frame->height = bd.Height;
    const std::size_t row = static_cast<std::size_t>(bd.Width) * 4;
    frame->pixels_rgba.resize(row * bd.Height);
    const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
    for (UINT y = 0; y < bd.Height; ++y) {
      memcpy(frame->pixels_rgba.data() + y * row,
             src + static_cast<std::size_t>(y) * mapped.RowPitch, row);
    }
    context->Unmap(g_staging, 0);
    if (g_mailbox != nullptr && g_mailbox->tryPublish(frame)) {
      g_last_capture_ns = frame->capture_time_ns;
      InterlockedIncrement64(&g_captures);
      published = true;
    }
  } while (false);
  if (back != nullptr) back->Release();
  if (context != nullptr) context->Release();
  if (device != nullptr) device->Release();
  (void)published;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync,
                                      UINT flags) {
  void** vtable = *reinterpret_cast<void***>(self);
  const VtableEntry* entry = nullptr;
  const LONG count = g_vtable_count;
  for (LONG i = 0; i < count && i < kMaxVtables; ++i) {
    if (g_vtables[i].vtable == vtable) {
      entry = &g_vtables[i];
      break;
    }
  }
  HRESULT hr = E_UNEXPECTED;
  if (entry != nullptr && entry->orig_present != nullptr && g_lock_ready) {
    CaptureAndPublish(self);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    bool preserved = false;
    bool checked = false;
    if (SUCCEEDED(self->GetDevice(__uuidof(ID3D11Device),
                                  reinterpret_cast<void**>(&device))) &&
        device != nullptr) {
      device->GetImmediateContext(&context);
      device->Release();
    }
    if (context != nullptr) {
      const mecvr::render::StateGuard before(context);
      hr = reinterpret_cast<PresentFn>(entry->orig_present)(self, sync, flags);
      preserved = before.verifyUnchanged();
      checked = true;
      context->Release();
    } else {
      hr = reinterpret_cast<PresentFn>(entry->orig_present)(self, sync, flags);
    }
    if (checked && !preserved) InterlockedIncrement64(&g_state_fail);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT count,
                                            UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags) {
  void** vtable = *reinterpret_cast<void***>(self);
  const VtableEntry* entry = nullptr;
  const LONG c = g_vtable_count;
  for (LONG i = 0; i < c && i < kMaxVtables; ++i) {
    if (g_vtables[i].vtable == vtable) {
      entry = &g_vtables[i];
      break;
    }
  }
  if (entry != nullptr && entry->orig_resize != nullptr) {
    return reinterpret_cast<ResizeBuffersFn>(entry->orig_resize)(
        self, count, width, height, format, flags);
  }
  return E_UNEXPECTED;
}
constexpr std::size_t kCreateSwapChainIndex = 10;
constexpr std::size_t kCreateSwapChainForHwndIndex = 15;

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

void** g_factory_vtables[kMaxVtables] = {};
void* g_factory_orig[kMaxVtables] = {};
void* g_factory_for_hwnd[kMaxVtables] = {};
volatile LONG g_factory_count = 0;

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* self,
                                              IUnknown* device,
                                              DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out) {
  for (LONG i = 0; i < g_factory_count; ++i) {
    if (g_factory_vtables[i] == *reinterpret_cast<void***>(self) &&
        g_factory_orig[i] != nullptr) {
      const HRESULT hr =
          reinterpret_cast<CreateSwapChainFn>(g_factory_orig[i])(self, device,
                                                                 desc, out);
      if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) HookVtable(*out);
      return hr;
    }
  }
  return E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output,
    IDXGISwapChain1** out) {
  for (LONG i = 0; i < g_factory_count; ++i) {
    if (g_factory_vtables[i] == *reinterpret_cast<void***>(self) &&
        g_factory_for_hwnd[i] != nullptr) {
      using ForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
          IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
          IDXGISwapChain1**);
      const HRESULT hr = reinterpret_cast<ForHwndFn>(
          g_factory_for_hwnd[i])(self, device, hwnd, desc, fs, output, out);
      if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) HookVtable(*out);
      return hr;
    }
  }
  return E_UNEXPECTED;
}

void HookFactoryVtable(IDXGIFactory* factory) {
  void** vtable = *reinterpret_cast<void***>(factory);
  for (LONG i = 0; i < g_factory_count; ++i) {
    if (g_factory_vtables[i] == vtable) return;
  }
  if (g_factory_count >= kMaxVtables) return;
  void* orig = nullptr;
  if (!PatchSlot(vtable, kCreateSwapChainIndex,
                 reinterpret_cast<void*>(&HookCreateSwapChain), &orig)) {
    return;
  }
  g_factory_vtables[g_factory_count] = vtable;
  g_factory_orig[g_factory_count] = orig;
  g_factory_for_hwnd[g_factory_count] = nullptr;
  // Index 15 (CreateSwapChainForHwnd) exists only on IDXGIFactory2+
  // vtables — probe before patching to avoid corrupting shorter tables.
  IDXGIFactory2* as_f2 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2),
                                        reinterpret_cast<void**>(&as_f2))) &&
      as_f2 != nullptr) {
    void* orig_fh = nullptr;
    if (PatchSlot(vtable, kCreateSwapChainForHwndIndex,
                  reinterpret_cast<void*>(&HookCreateSwapChainForHwnd),
                  &orig_fh)) {
      g_factory_for_hwnd[g_factory_count] = orig_fh;
    }
    as_f2->Release();
  }
  MemoryBarrier();
  InterlockedExchange(&g_factory_count, g_factory_count + 1);
}

// Creates throwaway own-device swapchains of several flavors so their
// (per-class shared) vtables get hooked — which simultaneously observes
// the game's pre-existing swapchains of the same classes. No scan, no
// race (T3B technique).
void HookKnownClasses(ID3D11Device* device, HWND hwnd) {
  if (device == nullptr || hwnd == nullptr) return;
  IDXGIDevice* dxgi_device = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory* factory = nullptr;
  if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                       reinterpret_cast<void**>(&dxgi_device))) &&
      dxgi_device != nullptr &&
      SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter != nullptr &&
      SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory),
                                   reinterpret_cast<void**>(&factory))) &&
      factory != nullptr) {
    HookFactoryVtable(factory);
    factory->Release();
  }
  if (dxgi_device != nullptr) dxgi_device->Release();
  if (adapter != nullptr) adapter->Release();

  DXGI_SWAP_CHAIN_DESC legacy{};
  legacy.BufferCount = 2;
  legacy.BufferDesc.Width = 64;
  legacy.BufferDesc.Height = 64;
  legacy.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  legacy.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  legacy.OutputWindow = hwnd;
  legacy.SampleDesc.Count = 1;
  legacy.Windowed = TRUE;
  legacy.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* legacy_chain = nullptr;
  IDXGIFactory* f2 = nullptr;
  if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory),
                                  reinterpret_cast<void**>(&f2))) &&
      f2 != nullptr) {
    HookFactoryVtable(f2);
    if (SUCCEEDED(f2->CreateSwapChain(device, &legacy, &legacy_chain)) &&
        legacy_chain != nullptr) {
      HookVtable(legacy_chain);
      legacy_chain->Release();
    }
    IDXGIFactory2* f2ex = nullptr;
    if (SUCCEEDED(f2->QueryInterface(__uuidof(IDXGIFactory2),
                                     reinterpret_cast<void**>(&f2ex))) &&
        f2ex != nullptr) {
      DXGI_SWAP_CHAIN_DESC1 d1{};
      d1.Width = 64;
      d1.Height = 64;
      d1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      d1.SampleDesc.Count = 1;
      d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      d1.BufferCount = 2;
      d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      IDXGISwapChain1* flip = nullptr;
      if (SUCCEEDED(f2ex->CreateSwapChainForHwnd(
              device, hwnd, &d1, nullptr, nullptr, &flip)) &&
          flip != nullptr) {
        HookVtable(flip);
        flip->Release();
      }
      // Flip-discard class: the modern default and the most likely
      // pre-existing game swapchain flavor not yet covered.
      DXGI_SWAP_CHAIN_DESC1 dd = d1;
      dd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      IDXGISwapChain1* discard = nullptr;
      if (SUCCEEDED(f2ex->CreateSwapChainForHwnd(
              device, hwnd, &dd, nullptr, nullptr, &discard)) &&
          discard != nullptr) {
        HookVtable(discard);
        discard->Release();
      }
      f2ex->Release();
    }
    f2->Release();
  }
}

void DumpStats(mecvr::openxr::XrFrameWorker& worker,
               mecvr::openxr::RealOpenXRBackend& backend, const char* tag) {
  const mecvr::openxr::M2bStats s = worker.stats();
  const mecvr::openxr::RealBackendDiagnostics d = backend.diagnostics();
  LogF(
      "%s presents=%lld captures=%lld skipped_thr=%lld state_fail=%lld "
      "submitted=%lld reused=%lld superseded=%lld empty=%lld upload_fail=%lld "
      "high_water=%llu rate_game=%.1f rate_xr=%.1f age_last=%lld age_max=%lld "
      "copy_ns=%lld up_ns=%lld acq_ns=%lld rel_ns=%lld wait_ns=%lld "
      "layer=%s space=%s end_fail=%llu",
      tag, g_presents, g_captures, g_capture_skipped_throttle, g_state_fail,
      s.submitted_new, s.reused, s.mailbox_superseded, s.empty_ticks,
      s.upload_failed, s.mailbox_high_water, s.present_rate_hz, s.xr_rate_hz,
      s.last_frame_age_ns, s.max_frame_age_ns, s.copy_ns, s.upload_ns,
      s.acquire_ns, s.release_ns, s.wait_ns, d.mono_layer.c_str(),
      d.mono_space.c_str(), d.end_failed);
}

unsigned __stdcall WorkerProc(void*) {
  LogF("m2b attached pid=%lu mode=LIVE-MONO-SUBMIT", g_pid);
  mecvr::openxr::RealOpenXRBackend backend;
  if (!backend.startup()) {
    LogF("FATAL: real XR backend startup failed: %s",
         backend.diagnostics().failure_reason.c_str());
    return 1;
  }
  auto* mailbox = new (std::nothrow) mecvr::openxr::FrameMailbox(2);
  if (mailbox == nullptr) {
    backend.shutdown();
    return 1;
  }
  g_mailbox = mailbox;

  ID3D11Device* own_device = nullptr;
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_9_1;
  WNDCLASSW wc{};
  wc.lpfnWndProc = &WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"MecvrM2bHidden";
  HWND hwnd = nullptr;
  if (RegisterClassW(&wc) != 0) {
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 64, 64, nullptr,
                           nullptr, wc.hInstance, nullptr);
  }
  if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                  0, nullptr, 0, D3D11_SDK_VERSION,
                                  &own_device, &obtained, nullptr)) &&
      own_device != nullptr) {
    HookKnownClasses(own_device, hwnd);
    own_device->Release();
  }
  if (hwnd != nullptr) DestroyWindow(hwnd);

  mecvr::openxr::XrFrameWorker worker(backend, *mailbox);
  LogF("m2b live: backend=%s hooks=%ld",
       backend.diagnostics().runtime_name.c_str(),
       static_cast<long>(g_vtable_count));
  std::int64_t last_dump = SteadyNs();
  while (WaitForSingleObject(g_own_shutdown, 0) != WAIT_OBJECT_0) {
    worker.run(180);  // ~2 s of XR ticks per chunk at 90 Hz.
    if (SteadyNs() - last_dump > 10000000000LL) {
      DumpStats(worker, backend, "m2b");
      last_dump = SteadyNs();
    }
  }
  DumpStats(worker, backend, "m2b-final");
  backend.shutdown();
  delete mailbox;
  g_mailbox = nullptr;
  return 0;
}

void BeginDetach(const char* reason) {
  if (InterlockedCompareExchange(&g_detaching, 1, 0) != 0) return;
  if (g_own_shutdown != nullptr) SetEvent(g_own_shutdown);
  if (g_worker_thread != nullptr) {
    WaitForSingleObject(g_worker_thread, kWorkerWaitMs);
    CloseHandle(g_worker_thread);
    g_worker_thread = nullptr;
  }
  for (LONG i = 0; i < g_vtable_count && i < kMaxVtables; ++i) {
    if (g_vtables[i].vtable == nullptr) continue;
    DWORD old = 0;
    if (VirtualProtect(&g_vtables[i].vtable[kPresentIndex], sizeof(void*),
                       PAGE_READWRITE, &old) != 0) {
      g_vtables[i].vtable[kPresentIndex] = g_vtables[i].orig_present;
      DWORD ignored = 0;
      VirtualProtect(&g_vtables[i].vtable[kPresentIndex], sizeof(void*), old,
                     &ignored);
    }
    if (VirtualProtect(&g_vtables[i].vtable[kResizeBuffersIndex],
                       sizeof(void*), PAGE_READWRITE, &old) != 0) {
      g_vtables[i].vtable[kResizeBuffersIndex] = g_vtables[i].orig_resize;
      DWORD ignored = 0;
      VirtualProtect(&g_vtables[i].vtable[kResizeBuffersIndex], sizeof(void*),
                     old, &ignored);
    }
  }
  for (LONG i = 0; i < g_factory_count && i < kMaxVtables; ++i) {
    if (g_factory_vtables[i] == nullptr) continue;
    DWORD old = 0;
    if (VirtualProtect(&g_factory_vtables[i][kCreateSwapChainIndex],
                       sizeof(void*), PAGE_READWRITE, &old) != 0) {
      g_factory_vtables[i][kCreateSwapChainIndex] = g_factory_orig[i];
      DWORD ignored = 0;
      VirtualProtect(&g_factory_vtables[i][kCreateSwapChainIndex],
                     sizeof(void*), old, &ignored);
    }
    if (g_factory_for_hwnd[i] != nullptr &&
        VirtualProtect(&g_factory_vtables[i][kCreateSwapChainForHwndIndex],
                       sizeof(void*), PAGE_READWRITE, &old) != 0) {
      g_factory_vtables[i][kCreateSwapChainForHwndIndex] =
          g_factory_for_hwnd[i];
      DWORD ignored = 0;
      VirtualProtect(&g_factory_vtables[i][kCreateSwapChainForHwndIndex],
                     sizeof(void*), old, &ignored);
    }
  }
  if (g_staging != nullptr) {
    g_staging->Release();
    g_staging = nullptr;
  }
  LogF("m2b detach (%s) presents=%lld captures=%lld state_fail=%lld",
       reason, g_presents, g_captures, g_state_fail);
  if (g_log != INVALID_HANDLE_VALUE) {
    CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
  }
  if (g_own_shutdown != nullptr) {
    CloseHandle(g_own_shutdown);
    g_own_shutdown = nullptr;
  }
  if (g_lock_ready) {
    DeleteCriticalSection(&g_lock);
    g_lock_ready = false;
  }
}

}  // namespace m2b_live

// DllMain MUST live at global scope: the CRT resolves it by external
// name, so any namespace membership (named or anonymous) hides it from
// the linker entry sequence. It delegates to m2b_live:: state.
using namespace m2b_live;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  (void)module;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    g_pid = GetCurrentProcessId();
    wchar_t temp[MAX_PATH];
    wchar_t path[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp) == 0) return FALSE;
    if (FAILED(StringCchPrintfW(path, MAX_PATH, L"%smecvr_m2b_%lu.log", temp,
                                g_pid))) {
      return FALSE;
    }
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE) return FALSE;
    wchar_t evname[128];
    if (FAILED(StringCchPrintfW(evname, 128, L"Local\\MECVR_M2B_SHUTDOWN_%lu",
                                g_pid))) {
      return FALSE;
    }
    g_own_shutdown = CreateEventW(nullptr, TRUE, FALSE, evname);
    if (g_own_shutdown == nullptr) return FALSE;
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
    g_worker_thread = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, &WorkerProc, nullptr, 0, nullptr));
    if (g_worker_thread == nullptr) {
      BeginDetach("worker-start-failed");
      return FALSE;
    }
    (void)reserved;
  } else if (reason == DLL_PROCESS_DETACH) {
    BeginDetach("process-detach");
  }
  return TRUE;
}

extern "C" {

__declspec(dllexport) void MecvrM2bShutdown(void) {
  if (m2b_live::g_own_shutdown != nullptr)
    SetEvent(m2b_live::g_own_shutdown);
}

}  // extern "C"
