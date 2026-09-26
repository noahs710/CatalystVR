// T3B M0C2 live-observation probe (render stream): live_probe.cc.
//
// Builds live_probe.dll, injected into the running game with the T6
// injector (mecvr_inject.exe --dll live_probe.dll --pid <game-pid>).
// OBSERVATION ONLY: no rendering modification, no camera/stereo/gameplay
// code (STOP S3). If this probe causes any hitch/crash/regression vs the
// pre-injection baseline, STOP S2: remove it, report evidence, do not
// escalate.
//
// Swapchain discovery approach: SHARED-VTABLE observation (permanent).
// The probe creates its own hidden-window D3D11 device + swapchains
// in-process (one legacy IDXGISwapChain plus IDXGISwapChain1..4 via QI)
// and vtable-patches Present[8]/ResizeBuffers[13] on each DISTINCT
// vtable, using the same two-step technique as src/render/vtable_hook
// (one page-protection change + one pointer-sized write per slot).
// COM interface vtables are per-class, not per-instance, so patching the
// probe's swapchain vtables simultaneously observes the game's
// pre-existing swapchain(s) without scanning process memory, without
// hunting for the game's swapchain pointer, and without racing game
// startup. This is why hooking IDXGIFactory::CreateSwapChain was
// rejected: injection happens long after game startup, so the game's
// swapchain already exists and a factory hook would never see it, while
// a memory scan for "the known swapchain" is fragile across builds.
// Per-call `self` identifies candidate swapchains; the most-frequent
// presenter is reported as primary. Reuses StateGuard (observer.h/.cc)
// per call against the GAME device's immediate context (queried from
// `self`), so state-preservation results describe the game, not the
// probe's device.
//
// After ONE frame capture to %TEMP% the probe keeps passive counting
// only (no unhook: unhooking a shared vtable under a live render thread
// is riskier than leaving a passive observer installed; process exit
// tears everything down).
//
// New file: existing src/render files are untouched.

#include <windows.h>

#include <dxgi1_5.h>

#include <d3d11.h>

#include <process.h>
#include <strsafe.h>

#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <new>

#include "render/observer.h"  // StateGuard only; Observer class unused here.

// IDXGISwapChain vtable slots (see observer.cc): Present[8],
// ResizeBuffers[13]. Identical for IDXGISwapChain1..4 (inherited).
constexpr std::size_t kPresentIndex = 8;
constexpr std::size_t kResizeBuffersIndex = 13;

namespace {

constexpr int kMaxVtables = 8;
constexpr int kMaxSwapchains = 16;
constexpr int kRingSize = 4096;
constexpr DWORD kWorkerWaitMs = 2000;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

struct VtableEntry {
  void** vtable = nullptr;
  void* orig_present = nullptr;
  void* orig_resize = nullptr;
};

struct SwapInfo {
  void* self = nullptr;
  volatile LONG64 presents = 0;
  UINT first_sync = 0;
  UINT first_flags = 0;
  volatile LONG64 flags_nonzero = 0;
  volatile LONG64 resizes = 0;
  volatile LONG64 state_fail = 0;
  DWORD first_tid = 0;
  // Last ResizeBuffers params.
  UINT rb_buffer_count = 0;
  UINT rb_width = 0;
  UINT rb_height = 0;
  UINT rb_format = 0;
  UINT rb_flags = 0;
  int rb_result = 0;
  // Swapchain desc, captured once in-hook (immutable for the swapchain).
  bool desc_ok = false;
  DXGI_SWAP_CHAIN_DESC desc = {};
};

struct FrameRecord {
  LONGLONG tick = 0;
  DWORD tid = 0;
  void* self = nullptr;
  UINT sync = 0;
  UINT flags = 0;
  int result = 0;
};

VtableEntry g_vtables[kMaxVtables];
volatile LONG g_vtable_count = 0;

SwapInfo g_swaps[kMaxSwapchains];
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

FrameRecord g_ring[kRingSize];
volatile LONG64 g_total_presents = 0;

volatile LONG g_capture_done = 0;   // 0 = not attempted, 1 = attempted once.
volatile LONG g_capture_ok = 0;     // 1 when pixel buffer ready.
volatile LONG g_capture_hr = 0;
UINT g_capture_width = 0;
UINT g_capture_height = 0;
UINT g_capture_format = 0;
void* g_capture_self = nullptr;
BYTE* g_capture_pixels = nullptr;  // W*H*4 BGRA, written by worker to BMP.

HANDLE g_worker_thread = nullptr;
HANDLE g_own_shutdown = nullptr;    // Local\MECVR_LIVEPROBE_SHUTDOWN_<pid>.
HANDLE g_runtime_shutdown = nullptr;  // T6 runtime event (secondary signal).
volatile LONG g_detaching = 0;
volatile LONG g_dump_requested = 0;
DWORD g_pid = 0;
LONGLONG g_qpf = 1;
wchar_t g_log_path[MAX_PATH] = {};
HANDLE g_log = INVALID_HANDLE_VALUE;
void* g_primary_hint = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  // NOTE: hwnd must be forwarded, never nullptr — DefWindowProcW on a null
  // window handle crashes the worker thread during window creation.
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void LogLine(const char* text) {
  if (g_log == INVALID_HANDLE_VALUE || text == nullptr) {
    return;
  }
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
  if (SUCCEEDED(StringCchVPrintfA(buf, _countof(buf), fmt, args))) {
    LogLine(buf);
  }
  va_end(args);
}

const char* FormatName(UINT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return "R8G8B8A8_UNORM_SRGB";
    default:
      return "other";
  }
}

SwapInfo* FindOrAddSwap(IDXGISwapChain* self) {
  // Caller holds g_lock.
  for (int i = 0; i < kMaxSwapchains; ++i) {
    if (g_swaps[i].self == self) {
      return &g_swaps[i];
    }
  }
  for (int i = 0; i < kMaxSwapchains; ++i) {
    if (g_swaps[i].self == nullptr) {
      g_swaps[i].self = self;
      if (SUCCEEDED(self->GetDesc(&g_swaps[i].desc))) {
        g_swaps[i].desc_ok = true;
      }
      return &g_swaps[i];
    }
  }
  return nullptr;  // Table full: count globally, skip per-swap detail.
}

const VtableEntry* FindVtable(void** vtable) {
  const LONG count = g_vtable_count;  // Aligned 32-bit volatile read.
  for (LONG i = 0; i < count && i < kMaxVtables; ++i) {
    if (g_vtables[i].vtable == vtable) {
      return &g_vtables[i];
    }
  }
  return nullptr;
}

void AttemptCapture(IDXGISwapChain* self, LONGLONG total) {
  // One attempt ever, after a short warmup. Runs on the game render
  // thread (its immediate context is current here). Copies pixels to a
  // heap buffer; the worker thread writes the BMP file.
  if (total < 30) {
    return;
  }
  if (InterlockedCompareExchange(&g_capture_done, 1, 0) != 0) {
    return;
  }
  HRESULT hr = E_FAIL;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11Texture2D* back = nullptr;
  ID3D11Texture2D* staging = nullptr;
  BYTE* pixels = nullptr;
  D3D11_TEXTURE2D_DESC bd = {};
  do {
    hr = self->GetDevice(__uuidof(ID3D11Device),
                         reinterpret_cast<void**>(&device));
    if (FAILED(hr) || device == nullptr) {
      break;
    }
    device->GetImmediateContext(&context);
    if (context == nullptr) {
      hr = E_UNEXPECTED;
      break;
    }
    hr = self->GetBuffer(0, __uuidof(ID3D11Texture2D),
                         reinterpret_cast<void**>(&back));
    if (FAILED(hr) || back == nullptr) {
      break;
    }
    back->GetDesc(&bd);
    if (bd.Width == 0 || bd.Height == 0 || bd.Width > 8192 ||
        bd.Height > 8192) {
      hr = E_UNEXPECTED;
      break;
    }
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = bd.Width;
    sd.Height = bd.Height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = bd.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr) || staging == nullptr) {
      break;
    }
    context->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
      break;
    }
    const size_t row = static_cast<size_t>(bd.Width) * 4;
    const size_t size = row * static_cast<size_t>(bd.Height);
    pixels = new (std::nothrow) BYTE[size];
    if (pixels == nullptr) {
      context->Unmap(staging, 0);
      hr = E_OUTOFMEMORY;
      break;
    }
    const BYTE* src = static_cast<const BYTE*>(mapped.pData);
    for (UINT y = 0; y < bd.Height; ++y) {
      memcpy(pixels + y * row,
             src + static_cast<size_t>(y) * mapped.RowPitch, row);
    }
    context->Unmap(staging, 0);
    hr = S_OK;
  } while (false);
  if (staging != nullptr) {
    staging->Release();
  }
  if (back != nullptr) {
    back->Release();
  }
  if (context != nullptr) {
    context->Release();
  }
  if (device != nullptr) {
    device->Release();
  }
  g_capture_hr = static_cast<LONG>(hr);
  if (SUCCEEDED(hr) && pixels != nullptr) {
    g_capture_pixels = pixels;
    g_capture_width = bd.Width;
    g_capture_height = bd.Height;
    g_capture_format = static_cast<UINT>(bd.Format);
    g_capture_self = self;
    InterlockedExchange(&g_capture_ok, 1);
  } else {
    delete[] pixels;
  }
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync,
                                      UINT flags) {
  const LONGLONG total = InterlockedIncrement64(&g_total_presents);
  LARGE_INTEGER qpc = {};
  QueryPerformanceCounter(&qpc);
  FrameRecord& slot = g_ring[static_cast<size_t>((total - 1) % kRingSize)];
  slot.tick = qpc.QuadPart;
  slot.tid = GetCurrentThreadId();
  slot.self = self;
  slot.sync = sync;
  slot.flags = flags;
  slot.result = 0;

  void** vtable = *reinterpret_cast<void***>(self);
  const VtableEntry* entry = FindVtable(vtable);

  HRESULT hr = E_UNEXPECTED;
  if (entry != nullptr && entry->orig_present != nullptr &&
      g_lock_ready) {
    // Per-swap bookkeeping under a brief lock; the original call runs
    // unlocked. StateGuard uses the GAME immediate context for `self`.
    EnterCriticalSection(&g_lock);
    SwapInfo* info = FindOrAddSwap(self);
    if (info != nullptr) {
      if (info->presents == 0) {
        info->first_sync = sync;
        info->first_flags = flags;
        info->first_tid = GetCurrentThreadId();
      }
      info->presents++;
      if (flags != 0) {
        info->flags_nonzero++;
      }
    }
    LeaveCriticalSection(&g_lock);

    AttemptCapture(self, total);

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
      hr = reinterpret_cast<PresentFn>(entry->orig_present)(self, sync,
                                                            flags);
      preserved = before.verifyUnchanged();
      checked = true;
      context->Release();
    } else {
      hr = reinterpret_cast<PresentFn>(entry->orig_present)(self, sync,
                                                            flags);
    }
    if (checked && !preserved && g_lock_ready) {
      EnterCriticalSection(&g_lock);
      SwapInfo* fail_info = FindOrAddSwap(self);
      if (fail_info != nullptr) {
        fail_info->state_fail++;
      }
      LeaveCriticalSection(&g_lock);
    }
  }
  slot.result = static_cast<int>(hr);
  return hr;
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self, UINT count,
                                            UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags) {
  void** vtable = *reinterpret_cast<void***>(self);
  const VtableEntry* entry = FindVtable(vtable);
  HRESULT hr = E_UNEXPECTED;
  if (entry != nullptr && entry->orig_resize != nullptr && g_lock_ready) {
    EnterCriticalSection(&g_lock);
    SwapInfo* info = FindOrAddSwap(self);
    if (info != nullptr) {
      info->resizes++;
      info->rb_buffer_count = count;
      info->rb_width = width;
      info->rb_height = height;
      info->rb_format = static_cast<UINT>(format);
      info->rb_flags = flags;
    }
    LeaveCriticalSection(&g_lock);

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
      hr = reinterpret_cast<ResizeBuffersFn>(entry->orig_resize)(
          self, count, width, height, format, flags);
      preserved = before.verifyUnchanged();
      checked = true;
      context->Release();
    } else {
      hr = reinterpret_cast<ResizeBuffersFn>(entry->orig_resize)(
          self, count, width, height, format, flags);
    }
    if (checked) {
      EnterCriticalSection(&g_lock);
      SwapInfo* info2 = FindOrAddSwap(self);
      if (info2 != nullptr) {
        info2->rb_result = static_cast<int>(hr);
        if (!preserved) {
          info2->state_fail++;
        }
      }
      LeaveCriticalSection(&g_lock);
    }
  }
  return hr;
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

// Records vtable (deduplicated), then patches slots 8/13. The entry is
// published before the slot write so a concurrent detour always finds
// its originals.
bool HookVtable(IDXGISwapChain* chain, char* log_buf, size_t log_size) {
  void** vtable = *reinterpret_cast<void***>(chain);
  for (LONG i = 0; i < g_vtable_count; ++i) {
    if (g_vtables[i].vtable == vtable) {
      StringCchPrintfA(log_buf, log_size,
                       "vtable %p already hooked, skipped", vtable);
      return true;
    }
  }
  if (g_vtable_count >= kMaxVtables) {
    StringCchPrintfA(log_buf, log_size, "vtable table full");
    return false;
  }
  VtableEntry& e = g_vtables[g_vtable_count];
  e.vtable = vtable;
  e.orig_present = vtable[kPresentIndex];
  e.orig_resize = vtable[kResizeBuffersIndex];
  MemoryBarrier();
  InterlockedExchange(&g_vtable_count, g_vtable_count + 1);
  void* check_p = nullptr;
  void* check_r = nullptr;
  if (!PatchSlot(vtable, kPresentIndex,
                 reinterpret_cast<void*>(&HookPresent), &check_p) ||
      !PatchSlot(vtable, kResizeBuffersIndex,
                 reinterpret_cast<void*>(&HookResizeBuffers), &check_r)) {
    StringCchPrintfA(log_buf, log_size, "VirtualProtect failed, gle=%lu",
                     static_cast<unsigned long>(GetLastError()));
    return false;
  }
  StringCchPrintfA(log_buf, log_size,
                   "hooked vtable %p orig_present=%p orig_resize=%p",
                   vtable, e.orig_present, e.orig_resize);
  return true;
}

void WriteCaptureBmp() {
  if (g_capture_ok == 0 || g_capture_pixels == nullptr ||
      g_capture_width == 0 || g_capture_height == 0) {
    return;
  }
  wchar_t path[MAX_PATH] = {};
  wchar_t temp[MAX_PATH] = {};
  const DWORD n =
      GetTempPathW(static_cast<DWORD>(_countof(temp)), temp);
  if (n == 0 || n >= _countof(temp)) {
    return;
  }
  StringCchPrintfW(path, _countof(path), L"%smecvr_capture_%lu.bmp", temp,
                   static_cast<unsigned long>(g_pid));
  HANDLE f =
      CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    LogF("capture: CreateFile failed gle=%lu",
         static_cast<unsigned long>(GetLastError()));
    return;
  }
  BITMAPFILEHEADER fh = {};
  BITMAPINFOHEADER ih = {};
  const DWORD row = g_capture_width * 4;
  const DWORD img = row * g_capture_height;
  ih.biSize = static_cast<DWORD>(sizeof(ih));
  ih.biWidth = static_cast<LONG>(g_capture_width);
  ih.biHeight = -static_cast<LONG>(g_capture_height);  // Top-down.
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  ih.biSizeImage = img;
  fh.bfType = 0x4D42;  // 'BM'.
  fh.bfOffBits =
      static_cast<DWORD>(sizeof(fh) + sizeof(ih));
  fh.bfSize = fh.bfOffBits + img;
  DWORD written = 0;
  WriteFile(f, &fh, static_cast<DWORD>(sizeof(fh)), &written, nullptr);
  WriteFile(f, &ih, static_cast<DWORD>(sizeof(ih)), &written, nullptr);
  WriteFile(f, g_capture_pixels, img, &written, nullptr);
  CloseHandle(f);
  char narrow[MAX_PATH * 2] = {};
  WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow,
                      static_cast<int>(sizeof(narrow)), nullptr, nullptr);
  LogF("capture: wrote %s (%ux%u fmt=%s)", narrow, g_capture_width,
       g_capture_height, FormatName(g_capture_format));
}

void DumpStats(const char* title) {
  const LONGLONG total = g_total_presents;
  LogF("--- %s (total_presents=%lld) ---", title, total);
  // Cadence from the ring in arrival order.
  LONGLONG valid = total < kRingSize ? total : kRingSize;
  if (valid >= 2 && g_qpf > 0) {
    double sum = 0.0;
    double min_ms = 1e18;
    double max_ms = 0.0;
    LONGLONG deltas = 0;
    LONGLONG prev = g_ring[static_cast<size_t>((total - valid) % kRingSize)]
                        .tick;
    for (LONGLONG i = total - valid + 1; i < total; ++i) {
      const LONGLONG t =
          g_ring[static_cast<size_t>(i % kRingSize)].tick;
      const double ms =
          static_cast<double>(t - prev) * 1000.0 / static_cast<double>(g_qpf);
      prev = t;
      if (ms < -1000.0 || ms > 1000.0) {
        continue;  // Ring overwrite race guard.
      }
      sum += ms;
      if (ms < min_ms) {
        min_ms = ms;
      }
      if (ms > max_ms) {
        max_ms = ms;
      }
      ++deltas;
    }
    if (deltas > 0) {
      const double mean = sum / static_cast<double>(deltas);
      LogF("cadence: n=%lld mean_ms=%.3f min_ms=%.3f max_ms=%.3f fps=%.2f",
           deltas, mean, min_ms, max_ms,
           mean > 0.0 ? 1000.0 / mean : 0.0);
    } else {
      LogLine("cadence: no valid deltas yet");
    }
    // Distinct render-thread ids in the window.
    DWORD tids[16] = {};
    int ntid = 0;
    for (LONGLONG i = total - valid; i < total; ++i) {
      const DWORD t =
          g_ring[static_cast<size_t>(i % kRingSize)].tid;
      bool seen = false;
      for (int k = 0; k < ntid; ++k) {
        if (tids[k] == t) {
          seen = true;
          break;
        }
      }
      if (!seen && ntid < 16) {
        tids[ntid++] = t;
      }
    }
    char tids_buf[256] = {};
    size_t pos = 0;
    for (int k = 0; k < ntid; ++k) {
      char tmp[32] = {};
      StringCchPrintfA(tmp, _countof(tmp), "%lu ",
                       static_cast<unsigned long>(tids[k]));
      StringCchCatA(tids_buf, _countof(tids_buf), tmp);
      pos += 1;
    }
    (void)pos;
    LogF("present_threads: n=%d tids=%s", ntid, tids_buf);
  } else {
    LogLine("cadence: waiting for more presents");
  }

  // Per-swapchain table; primary = most presents. Snapshot under the
  // lock, then log unlocked so file I/O never stalls the render thread.
  struct SwapSnapshot {
    void* self;
    LONGLONG presents;
    UINT first_sync;
    UINT first_flags;
    LONGLONG flags_nonzero;
    LONGLONG resizes;
    LONGLONG state_fail;
    DWORD first_tid;
    UINT rb_buffer_count;
    UINT rb_width;
    UINT rb_height;
    UINT rb_format;
    UINT rb_flags;
    int rb_result;
    bool desc_ok;
    DXGI_SWAP_CHAIN_DESC desc;
  };
  SwapSnapshot snap[kMaxSwapchains] = {};
  int nsnap = 0;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < kMaxSwapchains; ++i) {
    if (g_swaps[i].self == nullptr) {
      continue;
    }
    SwapSnapshot& s = snap[nsnap++];
    s.self = g_swaps[i].self;
    s.presents = g_swaps[i].presents;
    s.first_sync = g_swaps[i].first_sync;
    s.first_flags = g_swaps[i].first_flags;
    s.flags_nonzero = g_swaps[i].flags_nonzero;
    s.resizes = g_swaps[i].resizes;
    s.state_fail = g_swaps[i].state_fail;
    s.first_tid = g_swaps[i].first_tid;
    s.rb_buffer_count = g_swaps[i].rb_buffer_count;
    s.rb_width = g_swaps[i].rb_width;
    s.rb_height = g_swaps[i].rb_height;
    s.rb_format = g_swaps[i].rb_format;
    s.rb_flags = g_swaps[i].rb_flags;
    s.rb_result = g_swaps[i].rb_result;
    s.desc_ok = g_swaps[i].desc_ok;
    s.desc = g_swaps[i].desc;
  }
  LeaveCriticalSection(&g_lock);
  void* primary = nullptr;
  LONGLONG best = -1;
  for (int i = 0; i < nsnap; ++i) {
    if (snap[i].presents > best) {
      best = snap[i].presents;
      primary = snap[i].self;
    }
  }
  g_primary_hint = primary;
  for (int i = 0; i < nsnap; ++i) {
    const char* tag = (snap[i].self == primary) ? "PRIMARY" : "candidate";
    if (snap[i].desc_ok) {
      LogF("swapchain %p [%s]: presents=%lld first_sync=%u first_flags=%u "
           "flags_nonzero=%lld resizes=%lld state_fail=%lld first_tid=%lu "
           "w=%u h=%u fmt=%u(%s) windowed=%d hwnd=%p",
           snap[i].self, tag, snap[i].presents, snap[i].first_sync,
           snap[i].first_flags, snap[i].flags_nonzero, snap[i].resizes,
           snap[i].state_fail,
           static_cast<unsigned long>(snap[i].first_tid),
           snap[i].desc.BufferDesc.Width, snap[i].desc.BufferDesc.Height,
           static_cast<UINT>(snap[i].desc.BufferDesc.Format),
           FormatName(static_cast<UINT>(snap[i].desc.BufferDesc.Format)),
           static_cast<int>(snap[i].desc.Windowed),
           snap[i].desc.OutputWindow);
    } else {
      LogF("swapchain %p [%s]: presents=%lld (no desc)", snap[i].self,
           tag, snap[i].presents);
    }
    if (snap[i].resizes > 0) {
      LogF("  last ResizeBuffers: count=%u w=%u h=%u fmt=%u flags=%u hr=0x%08x",
           snap[i].rb_buffer_count, snap[i].rb_width, snap[i].rb_height,
           snap[i].rb_format, snap[i].rb_flags,
           static_cast<unsigned>(snap[i].rb_result));
    }
  }

  if (g_capture_done == 0) {
    LogLine("capture: pending (warmup)");
  } else if (g_capture_ok != 0) {
    LogF("capture: ok self=%p %ux%u fmt=%s hr=0x%08x", g_capture_self,
         g_capture_width, g_capture_height,
         FormatName(g_capture_format),
         static_cast<unsigned>(g_capture_hr));
  } else {
    LogF("capture: FAILED hr=0x%08x (one attempt only, no retry)",
         static_cast<unsigned>(g_capture_hr));
  }
  FlushFileBuffers(g_log);
}

void DescribePrimary() {
  if (g_primary_hint == nullptr) {
    return;
  }
  IDXGISwapChain* chain =
      static_cast<IDXGISwapChain*>(g_primary_hint);
  // HWND details.
  DXGI_SWAP_CHAIN_DESC desc = {};
  if (SUCCEEDED(chain->GetDesc(&desc)) && desc.OutputWindow != nullptr) {
    const HWND hwnd = desc.OutputWindow;
    wchar_t title[256] = {};
    wchar_t klass[256] = {};
    GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
    GetClassNameW(hwnd, klass, static_cast<int>(_countof(klass)));
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    RECT cr = {};
    GetClientRect(hwnd, &cr);
    char t[256] = {};
    char c[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, title, -1, t, static_cast<int>(sizeof(t)),
                        nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, klass, -1, c, static_cast<int>(sizeof(c)),
                        nullptr, nullptr);
    LogF("hwnd: %p title=\"%s\" class=\"%s\" visible=%d window=(%ld,%ld)-(%ld,%ld) client=%ldx%ld",
         hwnd, t, c, static_cast<int>(IsWindowVisible(hwnd)), rc.left,
         rc.top, rc.right, rc.bottom, cr.right - cr.left,
         cr.bottom - cr.top);
    BOOL fs = FALSE;
    IDXGIOutput* target = nullptr;
    const HRESULT fs_hr = chain->GetFullscreenState(&fs, &target);
    LogF("fullscreen: hr=0x%08x fullscreen=%d", static_cast<unsigned>(fs_hr),
         static_cast<int>(fs));
    if (target != nullptr) {
      DXGI_OUTPUT_DESC od = {};
      if (SUCCEEDED(target->GetDesc(&od))) {
        char dn[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, dn,
                            static_cast<int>(sizeof(dn)), nullptr, nullptr);
        LogF("output: name=\"%s\" desktop=(%ld,%ld)-(%ld,%ld) attached=%d",
             dn, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
             od.DesktopCoordinates.right, od.DesktopCoordinates.bottom,
             static_cast<int>(od.AttachedToDesktop));
      }
      target->Release();
    }
  }
  // Device / adapter details (AddRef-balanced, no context use here).
  ID3D11Device* device = nullptr;
  if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device),
                                 reinterpret_cast<void**>(&device))) &&
      device != nullptr) {
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D_FEATURE_LEVEL fl = device->GetFeatureLevel();
    LogF("device: ptr=%p immediate_context=%p feature_level=0x%x",
         device, context,
         static_cast<unsigned>(fl));
    if (context != nullptr) {
      context->Release();
    }
    IDXGIDevice* dxgi_dev = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(&dxgi_dev))) &&
        dxgi_dev != nullptr) {
      IDXGIAdapter* adapter = nullptr;
      if (SUCCEEDED(dxgi_dev->GetParent(
              __uuidof(IDXGIAdapter),
              reinterpret_cast<void**>(&adapter))) &&
          adapter != nullptr) {
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(adapter->GetDesc(&ad))) {
          char dd[256] = {};
          WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, dd,
                              static_cast<int>(sizeof(dd)), nullptr,
                              nullptr);
          LogF("adapter: \"%s\" vendor=0x%04x device=0x%04x "
               "luid=0x%08x:0x%08x",
               dd, static_cast<unsigned>(ad.VendorId),
               static_cast<unsigned>(ad.DeviceId),
               static_cast<unsigned>(ad.AdapterLuid.HighPart),
               static_cast<unsigned>(ad.AdapterLuid.LowPart));
        }
        adapter->Release();
      }
      dxgi_dev->Release();
    }
    device->Release();
  }
  FlushFileBuffers(g_log);
}

unsigned __stdcall WorkerProc(void*) {
  wchar_t temp[MAX_PATH] = {};
  const DWORD n = GetTempPathW(static_cast<DWORD>(_countof(temp)), temp);
  if (n == 0 || n >= _countof(temp)) {
    return 1;
  }
  StringCchPrintfW(g_log_path, _countof(g_log_path),
                   L"%smecvr_liveprobe_%lu.log", temp,
                   static_cast<unsigned long>(g_pid));
  g_log = CreateFileW(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_log == INVALID_HANDLE_VALUE) {
    return 1;
  }

  SYSTEMTIME st = {};
  GetLocalTime(&st);
  LARGE_INTEGER qpf = {};
  QueryPerformanceFrequency(&qpf);
  g_qpf = qpf.QuadPart;
  LogF("live_probe attached pid=%lu time=%04u-%02u-%02u %02u:%02u:%02u qpf=%lld",
       static_cast<unsigned long>(g_pid), st.wYear, st.wMonth, st.wDay,
       st.wHour, st.wMinute, st.wSecond, qpf.QuadPart);
  LogLine("mode=OBSERVE-ONLY (no rendering modification, no camera/stereo/gameplay)");

  wchar_t ev_name[128] = {};
  StringCchPrintfW(ev_name, _countof(ev_name),
                   L"Local\\MECVR_LIVEPROBE_SHUTDOWN_%lu",
                   static_cast<unsigned long>(g_pid));
  g_own_shutdown = CreateEventW(nullptr, TRUE, FALSE, ev_name);
  wchar_t rt_name[128] = {};
  StringCchPrintfW(rt_name, _countof(rt_name),
                   L"Local\\MECVR_RUNTIME_SHUTDOWN_%lu",
                   static_cast<unsigned long>(g_pid));
  g_runtime_shutdown = OpenEventW(SYNCHRONIZE, FALSE, rt_name);

  InitializeCriticalSection(&g_lock);
  g_lock_ready = true;

  // Own hidden device + swapchains: sources of DISTINCT COM vtables to
  // patch (shared per class with the game's swapchains).
  const HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW klass = {};
  klass.lpfnWndProc = &WndProc;
  klass.hInstance = inst;
  klass.lpszClassName = L"mecvr-liveprobe-hidden";
  HWND hwnd = nullptr;
  ID3D11Device* device = nullptr;
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_9_1;
  if (RegisterClassW(&klass) != 0) {
    hwnd = CreateWindowExW(0, klass.lpszClassName, L"mecvr-liveprobe",
                           WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, inst,
                           nullptr);
  }
  if (hwnd != nullptr) {
    const D3D_DRIVER_TYPE drivers[2] = {D3D_DRIVER_TYPE_HARDWARE,
                                        D3D_DRIVER_TYPE_WARP};
    for (int i = 0; i < 2 && device == nullptr; ++i) {
      D3D11CreateDevice(nullptr, drivers[i], nullptr, 0, nullptr, 0,
                        D3D11_SDK_VERSION, &device, &obtained, nullptr);
    }
  }
  if (device == nullptr || hwnd == nullptr) {
    LogF("FATAL: own device/hidden window unavailable (hwnd=%p device=%p)",
         hwnd, device);
    if (device != nullptr) {
      device->Release();
    }
    if (hwnd != nullptr) {
      DestroyWindow(hwnd);
    }
    CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
    return 1;
  }
  LogF("own device: ptr=%p feature_level=0x%x", device,
       static_cast<unsigned>(obtained));

  IDXGIDevice* dxgi_dev = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory* factory = nullptr;
  IDXGIFactory2* factory2 = nullptr;
  if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                       reinterpret_cast<void**>(&dxgi_dev))) &&
      dxgi_dev != nullptr &&
      SUCCEEDED(dxgi_dev->GetParent(
          __uuidof(IDXGIAdapter),
          reinterpret_cast<void**>(&adapter))) &&
      adapter != nullptr &&
      SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory),
                                   reinterpret_cast<void**>(&factory))) &&
      factory != nullptr) {
    factory->QueryInterface(__uuidof(IDXGIFactory2),
                            reinterpret_cast<void**>(&factory2));
  }

  char msg[256] = {};
  int hooked = 0;
  if (factory != nullptr) {
    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 2;
    d.BufferDesc.Width = 64;
    d.BufferDesc.Height = 64;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = hwnd;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* legacy = nullptr;
    if (SUCCEEDED(factory->CreateSwapChain(device, &d, &legacy)) &&
        legacy != nullptr) {
      if (HookVtable(legacy, msg, sizeof(msg))) {
        ++hooked;
      }
      LogF("legacy swapchain: self=%p %s", legacy, msg);
      // Keep alive for process lifetime (never released; owns nothing
      // game-visible). Intentionally leaked: releasing would destroy the
      // vtable anchor while hooks are installed.
      (void)legacy;
    } else {
      LogLine("legacy swapchain: CreateSwapChain failed");
    }
  }
  if (factory2 != nullptr) {
    DXGI_SWAP_CHAIN_DESC1 d1 = {};
    d1.Width = 64;
    d1.Height = 64;
    d1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d1.SampleDesc.Count = 1;
    d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d1.BufferCount = 2;
    d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(factory2->CreateSwapChainForHwnd(
            device, hwnd, &d1, nullptr, nullptr, &sc1)) &&
        sc1 != nullptr) {
      IUnknown* ifaces[4] = {};
      ifaces[0] = sc1;
      sc1->QueryInterface(__uuidof(IDXGISwapChain2),
                          reinterpret_cast<void**>(&ifaces[1]));
      sc1->QueryInterface(__uuidof(IDXGISwapChain3),
                          reinterpret_cast<void**>(&ifaces[2]));
      sc1->QueryInterface(__uuidof(IDXGISwapChain4),
                          reinterpret_cast<void**>(&ifaces[3]));
      for (int i = 0; i < 4; ++i) {
        if (ifaces[i] == nullptr) {
          continue;
        }
        IDXGISwapChain* as_base = nullptr;
        if (SUCCEEDED(ifaces[i]->QueryInterface(
                __uuidof(IDXGISwapChain),
                reinterpret_cast<void**>(&as_base))) &&
            as_base != nullptr) {
          if (HookVtable(as_base, msg, sizeof(msg))) {
            ++hooked;
          }
          LogF("swapchain iface %d: self=%p %s", i + 1, as_base, msg);
          as_base->Release();
        }
        if (i > 0) {
          ifaces[i]->Release();
        }
      }
      (void)sc1;  // Intentionally leaked (vtable anchor, see above).
    } else {
      LogLine("swapchain1: CreateSwapChainForHwnd failed");
    }
  }
  LogF("hooked %d distinct vtable(s); observing (passive counting)", hooked);
  if (factory2 != nullptr) {
    factory2->Release();
  }
  if (factory != nullptr) {
    factory->Release();
  }
  if (adapter != nullptr) {
    adapter->Release();
  }
  if (dxgi_dev != nullptr) {
    dxgi_dev->Release();
  }
  // device/hw/hwnd intentionally live until process exit (vtable anchors).
  (void)device;
  (void)hwnd;

  bool described = false;
  bool capture_written = false;
  HANDLE waits[2] = {g_own_shutdown, g_runtime_shutdown};
  const DWORD wait_count = (g_runtime_shutdown != nullptr) ? 2u : 1u;
  for (;;) {
    const DWORD w = WaitForMultipleObjects(wait_count, waits, FALSE,
                                           kWorkerWaitMs);
    if (g_detaching != 0) {
      break;
    }
    if (w == WAIT_OBJECT_0 || w == WAIT_OBJECT_0 + 1) {
      LogLine("shutdown event signaled: final dump (observation stays "
              "passive until process exit)");
      DumpStats("final");
      DescribePrimary();
      if (!capture_written) {
        WriteCaptureBmp();
        capture_written = true;
      }
      // Stay alive passively until detach; reduce log spam.
      waits[0] = g_own_shutdown;
      WaitForSingleObject(g_own_shutdown, INFINITE);
      if (g_detaching == 0) {
        // Explicit shutdown without detach: keep counting passively.
        continue;
      }
      break;
    }
    if (g_dump_requested != 0) {
      InterlockedExchange(&g_dump_requested, 0);
      DumpStats("explicit-dump");
      DescribePrimary();
      if (!capture_written && g_capture_ok != 0) {
        WriteCaptureBmp();
        capture_written = true;
      }
      continue;
    }
    DumpStats("periodic");
    if (!described && g_primary_hint != nullptr &&
        g_total_presents > 60) {
      DescribePrimary();
      described = true;
    }
    if (!capture_written && g_capture_ok != 0) {
      WriteCaptureBmp();
      capture_written = true;
    }
  }

  LogLine("detaching: final dump");
  DumpStats("detach-final");
  DescribePrimary();
  if (!capture_written) {
    WriteCaptureBmp();
  }
  if (g_capture_pixels != nullptr) {
    delete[] g_capture_pixels;
    g_capture_pixels = nullptr;
  }
  FlushFileBuffers(g_log);
  CloseHandle(g_log);
  g_log = INVALID_HANDLE_VALUE;
  return 0;
}

}  // namespace

extern "C" {

__declspec(dllexport) void MecvrProbeShutdown(void) {
  if (g_own_shutdown != nullptr) {
    SetEvent(g_own_shutdown);
  }
}

__declspec(dllexport) void MecvrProbeDump(void) {
  InterlockedExchange(&g_dump_requested, 1);
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(module);
      g_pid = GetCurrentProcessId();
      {
        unsigned thread_id = 0;
        const uintptr_t th = _beginthreadex(nullptr, 0, &WorkerProc,
                                            nullptr, 0, &thread_id);
        if (th != 0) {
          g_worker_thread = reinterpret_cast<HANDLE>(th);
        }
      }
      break;
    case DLL_PROCESS_DETACH:
      InterlockedExchange(&g_detaching, 1);
      if (g_own_shutdown != nullptr) {
        SetEvent(g_own_shutdown);
      }
      // Bounded wait only; hooks stay installed (passive until exit).
      if (g_worker_thread != nullptr) {
        WaitForSingleObject(g_worker_thread, 3000);
        CloseHandle(g_worker_thread);
        g_worker_thread = nullptr;
      }
      break;
    default:
      break;
  }
  return TRUE;
}
