// T10 M2A frame-capture + pre/post-HUD boundary probe (render stream).
//
// Builds m2a_probe.dll, injected into the running game with the T6
// injector (mecvr_inject.exe --dll m2a_probe.dll --pid <game-pid>).
// OBSERVATION ONLY: no rendering modification, no camera/stereo/gameplay
// code (STOP S3). If this probe causes any hitch/crash/regression vs the
// T3B baseline (~5.0 ms cadence, state_fail=0), STOP S2: remove it,
// report evidence, do not escalate.
//
// Technique: SHARED-VTABLE observation (permanent, as in live_probe.cc).
// The probe creates its own hidden-window D3D11 device + swapchains
// in-process and vtable-patches Present[8]/ResizeBuffers[13] on each
// DISTINCT swapchain vtable, plus OMSetRenderTargets[33] on the
// ID3D11DeviceContext vtable of its own immediate context. COM vtables
// are per-class, so this simultaneously observes the game's pre-existing
// objects with no memory scan and no startup race.
//
// What T10 adds over T3B (all read-only):
//  1. Present-time backbuffer identity: at each Present, snapshot the
//     bound RTVs (OMGetRenderTargets, up to 8 + DSV) and GetBuffer(0),
//     and test whether RTV0's resource IS the backbuffer. A sustained
//     match rate of 100% verifies the Present-time backbuffer is the
//     final-present output.
//  2. OMSetRenderTargets observation: per-call ring records RTV
//     dimensions, so the last fullscreen-size target bound before each
//     Present can be identified as the HUD-composite candidate.
//     Correctness of slot 33 is proven in-process by a sentinel: after
//     install the worker issues OMSetRenderTargets calls with a known
//     64x64 RTV on its OWN context and checks the ring recorded them.
//  3. Two time-separated captures (early title/idle vs a later state
//     reached with NO input injection) with pixel statistics + hashes,
//     to compare game states that appear on their own.
//  4. Hook-overhead accounting (QPC around in-hook work, original call
//     excluded) vs the T3B cadence baseline for the S2 gate.
//
// After both captures the probe keeps passive counting only (no unhook:
// unhooking a shared vtable under a live render thread is riskier than
// leaving a passive observer installed; process exit tears everything
// down).
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
// ID3D11DeviceContext layout: IUnknown[0..2], ID3D11DeviceChild[3..6]
// (GetDevice, GetPrivateData, SetPrivateData, SetPrivateDataInterface),
// then context methods VSSetConstantBuffers[7] ... GSSetSamplers[32],
// OMSetRenderTargets[33], OMSetRenderTargetsAndUnorderedAccessViews[34].
constexpr std::size_t kPresentIndex = 8;
constexpr std::size_t kResizeBuffersIndex = 13;
constexpr std::size_t kOmSetRenderTargetsIndex = 33;

namespace {

constexpr int kMaxVtables = 8;
constexpr int kMaxSwapchains = 16;
constexpr int kRingSize = 4096;
constexpr int kOmRingSize = 2048;
constexpr int kMaxRtvs = 8;
constexpr DWORD kWorkerWaitMs = 2000;
constexpr LONGLONG kCapture2AfterPresents = 30000;  // ~150 s at ~200 fps.

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using OmSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
    ID3D11DepthStencilView*);

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
  UINT rb_buffer_count = 0;
  UINT rb_width = 0;
  UINT rb_height = 0;
  UINT rb_format = 0;
  UINT rb_flags = 0;
  int rb_result = 0;
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
  // T10 additions: Present-time RTV snapshot summary.
  UINT rtv_count = 0;
  UINT rtv0_w = 0;
  UINT rtv0_h = 0;
  int rtv0_is_backbuffer = -1;  // 1 match, 0 mismatch, -1 unknown.
  LONGLONG fs_age_us = -1;      // Age of last fullscreen RTV set, -1 = none.
};

struct OmRecord {
  LONGLONG tick = 0;
  DWORD tid = 0;
  UINT num_views = 0;
  UINT w[kMaxRtvs] = {};
  UINT h[kMaxRtvs] = {};
  UINT fmt[kMaxRtvs] = {};
  int has_dsv = 0;
};

struct CaptureSlot {
  volatile LONG done = 0;
  volatile LONG ok = 0;
  volatile LONG hr = 0;
  UINT width = 0;
  UINT height = 0;
  UINT format = 0;
  void* self = nullptr;
  LONGLONG at_total = 0;
  BYTE* pixels = nullptr;
  unsigned long long fnv_hash = 0;
  double mean_luma = 0.0;
  double dark_frac = 0.0;
};

VtableEntry g_vtables[kMaxVtables];
volatile LONG g_vtable_count = 0;
void** g_ctx_vtable = nullptr;
void* g_orig_om = nullptr;

SwapInfo g_swaps[kMaxSwapchains];
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

FrameRecord g_ring[kRingSize];
volatile LONG64 g_total_presents = 0;

OmRecord g_om_ring[kOmRingSize];
volatile LONG64 g_om_calls = 0;
volatile LONG g_om_sentinel_ok = 0;  // 1 when own 64x64 sentinel observed.
volatile LONG g_om_hook_broken = 0;  // 1 when sentinel never observed.

volatile LONG64 g_rtv0_match = 0;
volatile LONG64 g_rtv0_mismatch = 0;
volatile LONG64 g_rtv_unknown = 0;
volatile LONG64 g_fs_found = 0;    // Presents with a prior fullscreen RTV.
volatile LONG64 g_fs_missing = 0;  // Presents with none in scan window.
volatile LONG64 g_hook_us_sum = 0;
volatile LONG64 g_hook_us_count = 0;

CaptureSlot g_cap1;
CaptureSlot g_cap2;

HANDLE g_worker_thread = nullptr;
HANDLE g_own_shutdown = nullptr;      // Local\MECVR_M2A_SHUTDOWN_<pid>.
HANDLE g_runtime_shutdown = nullptr;  // T6 runtime event (secondary signal).
volatile LONG g_detaching = 0;
volatile LONG g_dump_requested = 0;
DWORD g_pid = 0;
LONGLONG g_qpf = 1;
wchar_t g_log_path[MAX_PATH] = {};
HANDLE g_log = INVALID_HANDLE_VALUE;
void* g_primary_hint = nullptr;
UINT g_fs_w = 0;  // Primary fullscreen dims; 0 = unknown yet.
UINT g_fs_h = 0;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
  return nullptr;
}

const VtableEntry* FindVtable(void** vtable) {
  const LONG count = g_vtable_count;
  for (LONG i = 0; i < count && i < kMaxVtables; ++i) {
    if (g_vtables[i].vtable == vtable) {
      return &g_vtables[i];
    }
  }
  return nullptr;
}

void ResolveRtvDims(ID3D11RenderTargetView* rtv, UINT* w, UINT* h,
                    UINT* fmt) {
  *w = 0;
  *h = 0;
  *fmt = 0;
  if (rtv == nullptr) {
    return;
  }
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  if (res == nullptr) {
    return;
  }
  ID3D11Texture2D* tex = nullptr;
  if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&tex))) &&
      tex != nullptr) {
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    *w = d.Width;
    *h = d.Height;
    *fmt = static_cast<UINT>(d.Format);
    tex->Release();
  }
  res->Release();
}

void AnalyzePixels(BYTE* pixels, UINT width, UINT height,
                   unsigned long long* hash_out, double* mean_out,
                   double* dark_out) {
  // FNV-1a 64 over BGRA bytes + mean luma (Rec.601) + dark fraction.
  unsigned long long hash = 1469598103934665603ULL;
  unsigned long long luma_sum = 0;
  unsigned long long dark = 0;
  const size_t n =
      static_cast<size_t>(width) * static_cast<size_t>(height);
  for (size_t i = 0; i < n; ++i) {
    const BYTE b = pixels[i * 4 + 0];
    const BYTE g = pixels[i * 4 + 1];
    const BYTE r = pixels[i * 4 + 2];
    hash ^= static_cast<unsigned long long>(pixels[i * 4 + 0]);
    hash *= 1099511628211ULL;
    hash ^= static_cast<unsigned long long>(pixels[i * 4 + 1]);
    hash *= 1099511628211ULL;
    hash ^= static_cast<unsigned long long>(pixels[i * 4 + 2]);
    hash *= 1099511628211ULL;
    hash ^= static_cast<unsigned long long>(pixels[i * 4 + 3]);
    hash *= 1099511628211ULL;
    const unsigned lum = (299u * r + 587u * g + 114u * b) / 1000u;
    luma_sum += lum;
    if (lum < 8) {
      ++dark;
    }
  }
  *hash_out = hash;
  *mean_out = n > 0 ? static_cast<double>(luma_sum) / static_cast<double>(n)
                    : 0.0;
  *dark_out = n > 0 ? static_cast<double>(dark) / static_cast<double>(n)
                    : 0.0;
}

void AttemptCaptureSlot(CaptureSlot* slot, IDXGISwapChain* self,
                        LONGLONG total) {
  if (InterlockedCompareExchange(&slot->done, 1, 0) != 0) {
    return;
  }
  slot->at_total = total;
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
  slot->hr = static_cast<LONG>(hr);
  if (SUCCEEDED(hr) && pixels != nullptr) {
    slot->pixels = pixels;
    slot->width = bd.Width;
    slot->height = bd.Height;
    slot->format = static_cast<UINT>(bd.Format);
    slot->self = self;
    AnalyzePixels(pixels, bd.Width, bd.Height, &slot->fnv_hash,
                  &slot->mean_luma, &slot->dark_frac);
    InterlockedExchange(&slot->ok, 1);
  } else {
    delete[] pixels;
  }
}

// Scans the OM ring backwards (bounded window) for the most recent entry
// whose RTVs include a fullscreen-size target. Returns age in QPC ticks,
// or -1 when none found. Lock-free: tolerates torn concurrent writes.
LONGLONG FindLastFullscreenAge(LONGLONG now_tick, UINT fs_w, UINT fs_h) {
  if (fs_w == 0 || fs_h == 0) {
    return -1;
  }
  const LONGLONG calls = g_om_calls;
  LONGLONG window = calls < kOmRingSize ? calls : kOmRingSize;
  if (window > 1024) {
    window = 1024;  // Bound per-Present scan work on the render thread.
  }
  for (LONGLONG i = calls - 1; i >= calls - window; --i) {
    const OmRecord& rec =
        g_om_ring[static_cast<size_t>(i % kOmRingSize)];
    if (rec.tick == 0 || rec.tick > now_tick) {
      continue;
    }
    for (UINT k = 0; k < rec.num_views && k < kMaxRtvs; ++k) {
      if (rec.w[k] == fs_w && rec.h[k] == fs_h) {
        return now_tick - rec.tick;
      }
    }
  }
  return -1;
}

void STDMETHODCALLTYPE HookOmSetRenderTargets(ID3D11DeviceContext* self,
                                              UINT num_views,
                                              ID3D11RenderTargetView* const* rtvs,
                                              ID3D11DepthStencilView* dsv) {
  OmSetRenderTargetsFn original =
      reinterpret_cast<OmSetRenderTargetsFn>(g_orig_om);
  if (original != nullptr) {
    original(self, num_views, rtvs, dsv);
  }
  // Record AFTER the call: bounded, lock-free, no COM calls that can
  // fail the game (dims queries are read-only; failures yield 0s).
  const LONGLONG idx = InterlockedIncrement64(&g_om_calls) - 1;
  OmRecord& rec = g_om_ring[static_cast<size_t>(idx % kOmRingSize)];
  LARGE_INTEGER qpc = {};
  QueryPerformanceCounter(&qpc);
  rec.tick = qpc.QuadPart;
  rec.tid = GetCurrentThreadId();
  const UINT n = num_views > kMaxRtvs ? kMaxRtvs : num_views;
  rec.num_views = n;
  rec.has_dsv = (dsv != nullptr) ? 1 : 0;
  for (UINT i = 0; i < kMaxRtvs; ++i) {
    rec.w[i] = 0;
    rec.h[i] = 0;
    rec.fmt[i] = 0;
  }
  if (rtvs != nullptr) {
    for (UINT i = 0; i < n; ++i) {
      ResolveRtvDims(rtvs[i], &rec.w[i], &rec.h[i], &rec.fmt[i]);
    }
  }
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync,
                                      UINT flags) {
  LARGE_INTEGER hook_start = {};
  QueryPerformanceCounter(&hook_start);
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
  slot.rtv_count = 0;
  slot.rtv0_w = 0;
  slot.rtv0_h = 0;
  slot.rtv0_is_backbuffer = -1;
  slot.fs_age_us = -1;

  void** vtable = *reinterpret_cast<void***>(self);
  const VtableEntry* entry = FindVtable(vtable);

  HRESULT hr = E_UNEXPECTED;
  if (entry != nullptr && entry->orig_present != nullptr &&
      g_lock_ready) {
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

    if (total >= 30) {
      AttemptCaptureSlot(&g_cap1, self, total);
    }
    if (total >= kCapture2AfterPresents) {
      AttemptCaptureSlot(&g_cap2, self, total);
    }

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
      // Present-time RTV snapshot + backbuffer identity (read-only).
      ID3D11RenderTargetView* rtvs[kMaxRtvs] = {};
      ID3D11DepthStencilView* dsv = nullptr;
      context->OMGetRenderTargets(kMaxRtvs, rtvs, &dsv);
      UINT count = 0;
      for (UINT i = 0; i < kMaxRtvs && rtvs[i] != nullptr; ++i) {
        count++;
      }
      slot.rtv_count = count;
      if (count > 0) {
        UINT w = 0;
        UINT h = 0;
        UINT f = 0;
        (void)f;
        ResolveRtvDims(rtvs[0], &w, &h, &f);
        slot.rtv0_w = w;
        slot.rtv0_h = h;
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(self->GetBuffer(
                0, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&back))) &&
            back != nullptr) {
          ID3D11Resource* rtv_res = nullptr;
          rtvs[0]->GetResource(&rtv_res);
          ID3D11Resource* back_res = nullptr;
          back->QueryInterface(__uuidof(ID3D11Resource),
                               reinterpret_cast<void**>(&back_res));
          if (rtv_res != nullptr && back_res != nullptr) {
            slot.rtv0_is_backbuffer = (rtv_res == back_res) ? 1 : 0;
            if (rtv_res == back_res) {
              InterlockedIncrement64(&g_rtv0_match);
            } else {
              InterlockedIncrement64(&g_rtv0_mismatch);
            }
          } else {
            InterlockedIncrement64(&g_rtv_unknown);
          }
          if (rtv_res != nullptr) {
            rtv_res->Release();
          }
          if (back_res != nullptr) {
            back_res->Release();
          }
          back->Release();
        } else {
          if (back != nullptr) {
            back->Release();
          }
          InterlockedIncrement64(&g_rtv_unknown);
        }
      } else {
        InterlockedIncrement64(&g_rtv_unknown);
      }
      for (UINT i = 0; i < kMaxRtvs && rtvs[i] != nullptr; ++i) {
        rtvs[i]->Release();
      }
      if (dsv != nullptr) {
        dsv->Release();
      }
      // Last-fullscreen-RTV age (uses known primary dims if set).
      const LONGLONG age_ticks =
          FindLastFullscreenAge(qpc.QuadPart, g_fs_w, g_fs_h);
      if (age_ticks >= 0 && g_qpf > 0) {
        slot.fs_age_us =
            age_ticks * 1000000LL / g_qpf;
        InterlockedIncrement64(&g_fs_found);
      } else {
        InterlockedIncrement64(&g_fs_missing);
      }

      LARGE_INTEGER pre_end = {};
      QueryPerformanceCounter(&pre_end);
      const mecvr::render::StateGuard before(context);
      hr = reinterpret_cast<PresentFn>(entry->orig_present)(self, sync,
                                                            flags);
      preserved = before.verifyUnchanged();
      checked = true;
      if (g_qpf > 0) {
        // Added-work cost only: the original Present call runs after
        // pre_end, so only (pre_end - hook_start) is accumulated. The
        // post-original tail is one brief locked increment (untimed).
        InterlockedExchangeAdd64(
            &g_hook_us_sum,
            (pre_end.QuadPart - hook_start.QuadPart) * 1000000LL / g_qpf);
        InterlockedIncrement64(&g_hook_us_count);
      }
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

void WriteCaptureBmp(const CaptureSlot* slot, const wchar_t* tag) {
  if (slot->ok == 0 || slot->pixels == nullptr || slot->width == 0 ||
      slot->height == 0) {
    return;
  }
  wchar_t path[MAX_PATH] = {};
  wchar_t temp[MAX_PATH] = {};
  const DWORD n = GetTempPathW(static_cast<DWORD>(_countof(temp)), temp);
  if (n == 0 || n >= _countof(temp)) {
    return;
  }
  StringCchPrintfW(path, _countof(path), L"%smecvr_m2a_%s_%lu.bmp", temp,
                   tag, static_cast<unsigned long>(g_pid));
  HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    LogF("capture %ls: CreateFile failed gle=%lu", tag,
         static_cast<unsigned long>(GetLastError()));
    return;
  }
  BITMAPFILEHEADER fh = {};
  BITMAPINFOHEADER ih = {};
  const DWORD row = slot->width * 4;
  const DWORD img = row * slot->height;
  ih.biSize = static_cast<DWORD>(sizeof(ih));
  ih.biWidth = static_cast<LONG>(slot->width);
  ih.biHeight = -static_cast<LONG>(slot->height);
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  ih.biSizeImage = img;
  fh.bfType = 0x4D42;
  fh.bfOffBits = static_cast<DWORD>(sizeof(fh) + sizeof(ih));
  fh.bfSize = fh.bfOffBits + img;
  DWORD written = 0;
  WriteFile(f, &fh, static_cast<DWORD>(sizeof(fh)), &written, nullptr);
  WriteFile(f, &ih, static_cast<DWORD>(sizeof(ih)), &written, nullptr);
  WriteFile(f, slot->pixels, img, &written, nullptr);
  CloseHandle(f);
  char narrow[MAX_PATH * 2] = {};
  WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow,
                      static_cast<int>(sizeof(narrow)), nullptr, nullptr);
  LogF("capture %ls: wrote %s (%ux%u fmt=%s at_total=%lld "
       "fnv=0x%llx mean_luma=%.2f dark_frac=%.4f)",
       tag, narrow, slot->width, slot->height,
       FormatName(slot->format), slot->at_total, slot->fnv_hash,
       slot->mean_luma, slot->dark_frac);
}

void LogCaptureStatus(const CaptureSlot* slot, const char* tag) {
  if (slot->done == 0) {
    LogF("capture %s: pending", tag);
  } else if (slot->ok != 0) {
    LogF("capture %s: ok self=%p %ux%u fmt=%s at_total=%lld "
         "fnv=0x%llx mean_luma=%.2f dark_frac=%.4f hr=0x%08x",
         tag, slot->self, slot->width, slot->height,
         FormatName(slot->format), slot->at_total, slot->fnv_hash,
         slot->mean_luma, slot->dark_frac,
         static_cast<unsigned>(slot->hr));
  } else {
    LogF("capture %s: FAILED hr=0x%08x (one attempt only, no retry)", tag,
         static_cast<unsigned>(slot->hr));
  }
}

void UpdatePrimaryHint() {
  void* primary = nullptr;
  LONGLONG best = -1;
  EnterCriticalSection(&g_lock);
  for (int i = 0; i < kMaxSwapchains; ++i) {
    if (g_swaps[i].self != nullptr && g_swaps[i].presents > best) {
      best = g_swaps[i].presents;
      primary = g_swaps[i].self;
    }
    if (g_swaps[i].desc_ok && g_fs_w == 0) {
      g_fs_w = g_swaps[i].desc.BufferDesc.Width;
      g_fs_h = g_swaps[i].desc.BufferDesc.Height;
    }
  }
  LeaveCriticalSection(&g_lock);
  g_primary_hint = primary;
}

void DumpStats(const char* title) {
  UpdatePrimaryHint();
  const LONGLONG total = g_total_presents;
  LogF("--- %s (total_presents=%lld) ---", title, total);
  LONGLONG valid = total < kRingSize ? total : kRingSize;
  if (valid >= 2 && g_qpf > 0) {
    double sum = 0.0;
    double min_ms = 1e18;
    double max_ms = 0.0;
    LONGLONG deltas = 0;
    LONGLONG prev = g_ring[static_cast<size_t>((total - valid) % kRingSize)]
                        .tick;
    for (LONGLONG i = total - valid + 1; i < total; ++i) {
      const LONGLONG t = g_ring[static_cast<size_t>(i % kRingSize)].tick;
      const double ms =
          static_cast<double>(t - prev) * 1000.0 / static_cast<double>(g_qpf);
      prev = t;
      if (ms < -1000.0 || ms > 1000.0) {
        continue;
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
      LogF("cadence: n=%lld mean_ms=%.3f min_ms=%.3f max_ms=%.3f fps=%.2f "
           "(T3B baseline mean 5.000 ms; S2 gate: hitch/regression vs this)",
           deltas, mean, min_ms, max_ms,
           mean > 0.0 ? 1000.0 / mean : 0.0);
    } else {
      LogLine("cadence: no valid deltas yet");
    }
    DWORD tids[16] = {};
    int ntid = 0;
    for (LONGLONG i = total - valid; i < total; ++i) {
      const DWORD t = g_ring[static_cast<size_t>(i % kRingSize)].tid;
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
    for (int k = 0; k < ntid; ++k) {
      char tmp[32] = {};
      StringCchPrintfA(tmp, _countof(tmp), "%lu ",
                       static_cast<unsigned long>(tids[k]));
      StringCchCatA(tids_buf, _countof(tids_buf), tmp);
    }
    LogF("present_threads: n=%d tids=%s", ntid, tids_buf);
  } else {
    LogLine("cadence: waiting for more presents");
  }

  // Added-work overhead (original Present excluded by construction).
  const LONGLONG oh_n = g_hook_us_count;
  if (oh_n > 0) {
    LogF("present_hook_added_work: n=%lld mean_us=%.2f (pre-original "
         "section only: bookkeeping + RTV snapshot + OM-ring scan)",
         oh_n,
         static_cast<double>(g_hook_us_sum) / static_cast<double>(oh_n));
  }

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
  void* primary = g_primary_hint;
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

  // T10: backbuffer-identity verdict.
  const LONGLONG match = g_rtv0_match;
  const LONGLONG mismatch = g_rtv0_mismatch;
  const LONGLONG unknown = g_rtv_unknown;
  LogF("backbuffer_identity: rtv0_is_backbuffer match=%lld mismatch=%lld "
       "unknown=%lld (slot.rtv0 %ux%u)",
       match, mismatch, unknown, g_fs_w, g_fs_h);

  // T10: OMSetRenderTargets verdict.
  const LONGLONG om = g_om_calls;
  LogF("om_set_render_targets: calls=%lld sentinel_64x64=%s broken=%s",
       om, g_om_sentinel_ok != 0 ? "OBSERVED" : "not-observed",
       g_om_hook_broken != 0 ? "YES" : "no");
  if (om > 0) {
    // Distinct-tid count + top fullscreen-size histogram over the ring.
    DWORD om_tids[16] = {};
    int om_ntid = 0;
    LONGLONG win = om < kOmRingSize ? om : kOmRingSize;
    struct DimCount {
      UINT w;
      UINT h;
      LONGLONG n;
    };
    DimCount dims[32] = {};
    int ndims = 0;
    LONGLONG fs_sets = 0;
    for (LONGLONG i = om - win; i < om; ++i) {
      const OmRecord& rec = g_om_ring[static_cast<size_t>(i % kOmRingSize)];
      if (rec.tick == 0) {
        continue;
      }
      bool seen = false;
      for (int k = 0; k < om_ntid; ++k) {
        if (om_tids[k] == rec.tid) {
          seen = true;
          break;
        }
      }
      if (!seen && om_ntid < 16) {
        om_tids[om_ntid++] = rec.tid;
      }
      for (UINT k = 0; k < rec.num_views && k < kMaxRtvs; ++k) {
        if (rec.w[k] == 0) {
          continue;
        }
        if (g_fs_w != 0 && rec.w[k] == g_fs_w && rec.h[k] == g_fs_h) {
          fs_sets++;
        }
        int found = -1;
        for (int d = 0; d < ndims; ++d) {
          if (dims[d].w == rec.w[k] && dims[d].h == rec.h[k]) {
            found = d;
            break;
          }
        }
        if (found >= 0) {
          dims[found].n++;
        } else if (ndims < 32) {
          dims[ndims].w = rec.w[k];
          dims[ndims].h = rec.h[k];
          dims[ndims].n = 1;
          ndims++;
        }
      }
    }
    char om_tids_buf[256] = {};
    for (int k = 0; k < om_ntid; ++k) {
      char tmp[32] = {};
      StringCchPrintfA(tmp, _countof(tmp), "%lu ",
                       static_cast<unsigned long>(om_tids[k]));
      StringCchCatA(om_tids_buf, _countof(om_tids_buf), tmp);
    }
    LogF("om_threads: n=%d tids=%s", om_ntid, om_tids_buf);
    for (int d = 0; d < ndims; ++d) {
      LogF("om_rtv_size: %ux%u sets=%lld%s", dims[d].w, dims[d].h,
           dims[d].n,
           (g_fs_w != 0 && dims[d].w == g_fs_w && dims[d].h == g_fs_h)
               ? " [FULLSCREEN]"
               : "");
    }
    LogF("om_fullscreen_sets_in_window=%lld fs_found=%lld fs_missing=%lld",
         fs_sets, static_cast<LONGLONG>(g_fs_found),
         static_cast<LONGLONG>(g_fs_missing));
    // Mean last-fullscreen age over the Present ring window.
    double age_sum = 0.0;
    LONGLONG age_n = 0;
    double age_max = 0.0;
    LONGLONG pwin = total < kRingSize ? total : kRingSize;
    for (LONGLONG i = total - pwin; i < total; ++i) {
      const FrameRecord& fr = g_ring[static_cast<size_t>(i % kRingSize)];
      if (fr.tick == 0 || fr.fs_age_us < 0) {
        continue;
      }
      age_sum += static_cast<double>(fr.fs_age_us);
      if (static_cast<double>(fr.fs_age_us) > age_max) {
        age_max = static_cast<double>(fr.fs_age_us);
      }
      age_n++;
    }
    if (age_n > 0) {
      LogF("last_fullscreen_before_present: n=%lld mean_age_us=%.1f "
           "max_age_us=%.1f",
           age_n, age_sum / static_cast<double>(age_n), age_max);
    } else {
      LogLine("last_fullscreen_before_present: no samples in window");
    }
  }

  LogCaptureStatus(&g_cap1, "cap1-early");
  LogCaptureStatus(&g_cap2, "cap2-late");
  if (g_cap1.ok != 0 && g_cap2.ok != 0) {
    LogF("capture_compare: %s (fnv1=0x%llx fnv2=0x%llx "
         "mean1=%.2f mean2=%.2f dark1=%.4f dark2=%.4f)",
         g_cap1.fnv_hash == g_cap2.fnv_hash ? "IDENTICAL" : "DIFFERENT",
         g_cap1.fnv_hash, g_cap2.fnv_hash, g_cap1.mean_luma,
         g_cap2.mean_luma, g_cap1.dark_frac, g_cap2.dark_frac);
  }
  FlushFileBuffers(g_log);
}

void DescribePrimary() {
  if (g_primary_hint == nullptr) {
    return;
  }
  IDXGISwapChain* chain = static_cast<IDXGISwapChain*>(g_primary_hint);
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
  ID3D11Device* device = nullptr;
  if (SUCCEEDED(chain->GetDevice(__uuidof(ID3D11Device),
                                 reinterpret_cast<void**>(&device))) &&
      device != nullptr) {
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const D3D_FEATURE_LEVEL fl = device->GetFeatureLevel();
    LogF("device: ptr=%p immediate_context=%p feature_level=0x%x",
         device, context, static_cast<unsigned>(fl));
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
                              static_cast<int>(sizeof(dd)), nullptr, nullptr);
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

// Sentinel falsification test for the OMSetRenderTargets slot: issue calls
// with a known 64x64 RTV on our OWN context, then scan the ring for them.
// Runs on the worker thread; the game render thread is unaffected.
void RunOmSentinel(ID3D11DeviceContext* own_context,
                   ID3D11RenderTargetView* sentinel_rtv) {
  const LONGLONG before = g_om_calls;
  for (int i = 0; i < 3; ++i) {
    own_context->OMSetRenderTargets(1, &sentinel_rtv, nullptr);
  }
  Sleep(200);  // Same-thread hook ran synchronously; grace only.
  const LONGLONG after = g_om_calls;
  bool seen = false;
  LONGLONG win = after - before;
  if (win < 0) {
    win = 0;
  }
  if (win > kOmRingSize) {
    win = kOmRingSize;
  }
  for (LONGLONG i = after - win; i < after; ++i) {
    const OmRecord& rec = g_om_ring[static_cast<size_t>(i % kOmRingSize)];
    if (rec.tick != 0 && rec.num_views == 1 && rec.w[0] == 64 &&
        rec.h[0] == 64) {
      seen = true;
      break;
    }
  }
  if (seen) {
    InterlockedExchange(&g_om_sentinel_ok, 1);
    LogF("om_sentinel: OBSERVED own 64x64 RTV calls=%lld (slot 33 "
         "confirmed)", after - before);
  } else {
    // Do not set broken yet: game OM traffic may have been recorded
    // instead if the slot is shared and busy. Retry once after 2 s.
    Sleep(2000);
    for (LONGLONG i = before; i < g_om_calls; ++i) {
      const OmRecord& rec =
          g_om_ring[static_cast<size_t>(i % kOmRingSize)];
      if (rec.tick != 0 && rec.num_views == 1 && rec.w[0] == 64 &&
          rec.h[0] == 64) {
        seen = true;
        break;
      }
    }
    if (seen) {
      InterlockedExchange(&g_om_sentinel_ok, 1);
      LogF("om_sentinel: OBSERVED on retry (slot 33 confirmed)");
    } else {
      InterlockedExchange(&g_om_hook_broken, 1);
      LogF("om_sentinel: NOT OBSERVED (slot may be wrong; OM evidence "
           "will be marked UNRESOLVED, Present-snapshot evidence stands)");
    }
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
                   L"%smecvr_m2a_%lu.log", temp,
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
  LogF("m2a_probe attached pid=%lu time=%04u-%02u-%02u %02u:%02u:%02u qpf=%lld",
       static_cast<unsigned long>(g_pid), st.wYear, st.wMonth, st.wDay,
       st.wHour, st.wMinute, st.wSecond, qpf.QuadPart);
  LogLine("mode=OBSERVE-ONLY (no rendering modification, no camera/stereo/gameplay)");
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, static_cast<DWORD>(_countof(exe_path)));
  char exe_narrow[MAX_PATH * 2] = {};
  WideCharToMultiByte(CP_UTF8, 0, exe_path, -1, exe_narrow,
                      static_cast<int>(sizeof(exe_narrow)), nullptr, nullptr);
  LogF("exe: %s", exe_narrow);
  wchar_t sysdir[MAX_PATH] = {};
  GetSystemDirectoryW(sysdir, static_cast<DWORD>(_countof(sysdir)));
  const wchar_t* mods[2] = {L"dxgi.dll", L"d3d11.dll"};
  for (int i = 0; i < 2; ++i) {
    wchar_t mod_path[MAX_PATH] = {};
    if (GetModuleFileNameW(GetModuleHandleW(mods[i]), mod_path,
                           static_cast<DWORD>(_countof(mod_path))) > 0) {
      char mod_narrow[MAX_PATH * 2] = {};
      WideCharToMultiByte(CP_UTF8, 0, mod_path, -1, mod_narrow,
                          static_cast<int>(sizeof(mod_narrow)), nullptr,
                          nullptr);
      LogF("module: %s", mod_narrow);
    }
  }

  wchar_t ev_name[128] = {};
  StringCchPrintfW(ev_name, _countof(ev_name),
                   L"Local\\MECVR_M2A_SHUTDOWN_%lu",
                   static_cast<unsigned long>(g_pid));
  g_own_shutdown = CreateEventW(nullptr, TRUE, FALSE, ev_name);
  wchar_t rt_name[128] = {};
  StringCchPrintfW(rt_name, _countof(rt_name),
                   L"Local\\MECVR_RUNTIME_SHUTDOWN_%lu",
                   static_cast<unsigned long>(g_pid));
  g_runtime_shutdown = OpenEventW(SYNCHRONIZE, FALSE, rt_name);

  InitializeCriticalSection(&g_lock);
  g_lock_ready = true;

  const HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW klass = {};
  klass.lpfnWndProc = &WndProc;
  klass.hInstance = inst;
  klass.lpszClassName = L"mecvr-m2a-hidden";
  HWND hwnd = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* own_context = nullptr;
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_9_1;
  if (RegisterClassW(&klass) != 0) {
    hwnd = CreateWindowExW(0, klass.lpszClassName, L"mecvr-m2a",
                           WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, inst,
                           nullptr);
  }
  if (hwnd != nullptr) {
    const D3D_DRIVER_TYPE drivers[2] = {D3D_DRIVER_TYPE_HARDWARE,
                                        D3D_DRIVER_TYPE_WARP};
    for (int i = 0; i < 2 && device == nullptr; ++i) {
      D3D11CreateDevice(nullptr, drivers[i], nullptr, 0, nullptr, 0,
                        D3D11_SDK_VERSION, &device, &obtained, &own_context);
    }
  }
  if (device == nullptr || hwnd == nullptr) {
    LogF("FATAL: own device/hidden window unavailable (hwnd=%p device=%p)",
         hwnd, device);
    if (own_context != nullptr) {
      own_context->Release();
    }
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
  LogF("own device: ptr=%p feature_level=0x%x context=%p", device,
       static_cast<unsigned>(obtained), own_context);

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
      (void)legacy;  // Intentionally leaked: vtable anchor.
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
      (void)sc1;  // Intentionally leaked (vtable anchor).
    } else {
      LogLine("swapchain1: CreateSwapChainForHwnd failed");
    }
  }
  LogF("hooked %d distinct swapchain vtable(s); observing (passive counting)",
       hooked);

  // Context-vtable hook (shared per class with the game context).
  ID3D11RenderTargetView* sentinel_rtv = nullptr;
  bool om_hooked = false;
  if (own_context != nullptr) {
    // Sentinel RTV (64x64) on the OWN device for the slot-33 self-test.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* sentinel_tex = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &sentinel_tex)) &&
        sentinel_tex != nullptr) {
      if (SUCCEEDED(device->CreateRenderTargetView(sentinel_tex, nullptr,
                                                   &sentinel_rtv))) {
        void** ctx_vtable = *reinterpret_cast<void***>(own_context);
        g_ctx_vtable = ctx_vtable;
        void* orig = nullptr;
        if (PatchSlot(ctx_vtable, kOmSetRenderTargetsIndex,
                      reinterpret_cast<void*>(&HookOmSetRenderTargets),
                      &orig) &&
            orig != nullptr) {
          g_orig_om = orig;
          om_hooked = true;
          LogF("context vtable %p slot %u hooked orig_om=%p",
               ctx_vtable,
               static_cast<unsigned>(kOmSetRenderTargetsIndex), orig);
        } else {
          LogLine("context vtable: PatchSlot failed (OM hook skipped)");
        }
      } else {
        LogLine("sentinel RTV creation failed (OM hook skipped)");
      }
      if (sentinel_tex != nullptr) {
        sentinel_tex->Release();
      }
    } else {
      LogLine("sentinel texture creation failed (OM hook skipped)");
    }
  }
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
  (void)device;  // Anchors stay alive until process exit.
  (void)hwnd;

  if (om_hooked && own_context != nullptr && sentinel_rtv != nullptr) {
    RunOmSentinel(own_context, sentinel_rtv);
  } else if (!om_hooked) {
    InterlockedExchange(&g_om_hook_broken, 1);
    LogLine("om_sentinel: hook not installed; OM evidence UNRESOLVED");
  }
  if (sentinel_rtv != nullptr) {
    sentinel_rtv->Release();
    sentinel_rtv = nullptr;
  }

  bool described = false;
  bool caps_written = false;
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
      if (!caps_written) {
        WriteCaptureBmp(&g_cap1, L"cap1");
        WriteCaptureBmp(&g_cap2, L"cap2");
        caps_written = true;
      }
      FlushFileBuffers(g_log);
      if (g_detaching == 0) {
        waits[0] = g_own_shutdown;
        WaitForSingleObject(g_own_shutdown, INFINITE);
        if (g_detaching == 0) {
          continue;
        }
      }
      break;
    }
    if (g_dump_requested != 0) {
      InterlockedExchange(&g_dump_requested, 0);
      DumpStats("explicit-dump");
      DescribePrimary();
      if (!caps_written && g_cap1.ok != 0 && g_cap2.ok != 0) {
        WriteCaptureBmp(&g_cap1, L"cap1");
        WriteCaptureBmp(&g_cap2, L"cap2");
        caps_written = true;
      }
      continue;
    }
    DumpStats("periodic");
    if (!described && g_primary_hint != nullptr &&
        g_total_presents > 60) {
      DescribePrimary();
      described = true;
    }
    if (!caps_written && g_cap1.ok != 0 && g_cap2.ok != 0) {
      WriteCaptureBmp(&g_cap1, L"cap1");
      WriteCaptureBmp(&g_cap2, L"cap2");
      caps_written = true;
    }
  }

  LogLine("detaching: final dump");
  DumpStats("detach-final");
  DescribePrimary();
  if (!caps_written) {
    WriteCaptureBmp(&g_cap1, L"cap1");
    WriteCaptureBmp(&g_cap2, L"cap2");
  }
  if (g_cap1.pixels != nullptr) {
    delete[] g_cap1.pixels;
    g_cap1.pixels = nullptr;
  }
  if (g_cap2.pixels != nullptr) {
    delete[] g_cap2.pixels;
    g_cap2.pixels = nullptr;
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

