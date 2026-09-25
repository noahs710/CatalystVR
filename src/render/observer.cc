#include "render/observer.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

namespace mecvr::render {

namespace {

// IDXGISwapChain vtable slot layout: IUnknown[0..2], IDXGIObject[3..6],
// IDXGIDeviceSubObject[7 (::GetDevice)], then IDXGISwapChain methods:
// Present[8], GetBuffer[9], SetFullscreenState[10], GetFullscreenState[11],
// GetDesc[12], ResizeBuffers[13], ResizeTarget[14], GetContainingOutput[15],
// GetFrameStatistics[16], GetLastPresentCount[17].
constexpr std::size_t kPresentIndex = 8;
constexpr std::size_t kResizeBuffersIndex = 13;

std::mutex g_mutex;
Observer* g_active = nullptr;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

void Snapshot(ID3D11DeviceContext* context, ID3D11RenderTargetView** rtv,
              ID3D11DepthStencilView** dsv,
              ID3D11RasterizerState** rasterizer, ID3D11BlendState** blend,
              float blend_factor[4], UINT* sample_mask,
              ID3D11DepthStencilState** depth_stencil, UINT* stencil_ref,
              UINT* viewport_count, D3D11_VIEWPORT* viewport) {
  context->OMGetRenderTargets(1, rtv, dsv);
  context->RSGetState(rasterizer);
  context->OMGetBlendState(blend, blend_factor, sample_mask);
  context->OMGetDepthStencilState(depth_stencil, stencil_ref);
  *viewport_count = 1;
  context->RSGetViewports(viewport_count, viewport);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* self, UINT sync_interval,
                                       UINT flags) {
  Observer* active = nullptr;
  {
    const std::lock_guard<std::mutex> lock(g_mutex);
    active = g_active;
  }
  if (active == nullptr) {
    return E_UNEXPECTED;
  }
  return static_cast<HRESULT>(
      active->hookPresent(self, sync_interval, flags));
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* self,
                                            UINT buffer_count, UINT width,
                                            UINT height, DXGI_FORMAT format,
                                            UINT flags) {
  Observer* active = nullptr;
  {
    const std::lock_guard<std::mutex> lock(g_mutex);
    active = g_active;
  }
  if (active == nullptr) {
    return E_UNEXPECTED;
  }
  return static_cast<HRESULT>(active->hookResizeBuffers(
      self, buffer_count, width, height, static_cast<unsigned int>(format),
      flags));
}

}  // namespace

StateGuard::StateGuard(ID3D11DeviceContext* context) : context_(context) {
  float factor[4] = {};
  UINT mask = 0;
  UINT stencil_ref = 0;
  UINT viewport_count = 0;
  D3D11_VIEWPORT viewport = {};
  Snapshot(context_, &rtv_, &dsv_, &rasterizer_, &blend_, factor, &mask,
           &depth_stencil_, &stencil_ref, &viewport_count, &viewport);
  for (int i = 0; i < 4; ++i) {
    blend_factor_[i] = factor[i];
  }
  sample_mask_ = mask;
  stencil_ref_ = stencil_ref;
  viewport_count_ = viewport_count;
  viewport_x_ = viewport.TopLeftX;
  viewport_y_ = viewport.TopLeftY;
  viewport_width_ = viewport.Width;
  viewport_height_ = viewport.Height;
  viewport_min_depth_ = viewport.MinDepth;
  viewport_max_depth_ = viewport.MaxDepth;
}

StateGuard::~StateGuard() {
  if (rtv_ != nullptr) {
    rtv_->Release();
  }
  if (dsv_ != nullptr) {
    dsv_->Release();
  }
  if (rasterizer_ != nullptr) {
    rasterizer_->Release();
  }
  if (blend_ != nullptr) {
    blend_->Release();
  }
  if (depth_stencil_ != nullptr) {
    depth_stencil_->Release();
  }
}

bool StateGuard::verifyUnchanged() const {
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11RasterizerState* rasterizer = nullptr;
  ID3D11BlendState* blend = nullptr;
  ID3D11DepthStencilState* depth_stencil = nullptr;
  float factor[4] = {};
  UINT mask = 0;
  UINT stencil_ref = 0;
  UINT viewport_count = 0;
  D3D11_VIEWPORT viewport = {};
  Snapshot(context_, &rtv, &dsv, &rasterizer, &blend, factor, &mask,
           &depth_stencil, &stencil_ref, &viewport_count, &viewport);

  bool ok = true;
  if (rtv != rtv_) {
    mismatch_ = "render-target view changed";
    ok = false;
  } else if (dsv != dsv_) {
    mismatch_ = "depth-stencil view changed";
    ok = false;
  } else if (rasterizer != rasterizer_) {
    mismatch_ = "rasterizer state changed";
    ok = false;
  } else if (blend != blend_) {
    mismatch_ = "blend state changed";
    ok = false;
  } else if (depth_stencil != depth_stencil_) {
    mismatch_ = "depth-stencil state changed";
    ok = false;
  } else if (mask != sample_mask_ || stencil_ref != stencil_ref_) {
    mismatch_ = "sample mask or stencil ref changed";
    ok = false;
  } else if (viewport_count != viewport_count_) {
    mismatch_ = "viewport count changed";
    ok = false;
  } else if (viewport.TopLeftX != viewport_x_ ||
             viewport.TopLeftY != viewport_y_ ||
             viewport.Width != viewport_width_ ||
             viewport.Height != viewport_height_ ||
             viewport.MinDepth != viewport_min_depth_ ||
             viewport.MaxDepth != viewport_max_depth_) {
    mismatch_ = "viewport changed";
    ok = false;
  } else {
    for (int i = 0; i < 4; ++i) {
      if (factor[i] != blend_factor_[i]) {
        mismatch_ = "blend factor changed";
        ok = false;
        break;
      }
    }
  }

  if (rtv != nullptr) {
    rtv->Release();
  }
  if (dsv != nullptr) {
    dsv->Release();
  }
  if (rasterizer != nullptr) {
    rasterizer->Release();
  }
  if (blend != nullptr) {
    blend->Release();
  }
  if (depth_stencil != nullptr) {
    depth_stencil->Release();
  }
  return ok;
}

bool Observer::install(IDXGISwapChain* swapchain) {
  if (swapchain == nullptr) {
    return false;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  if (installed_flag_) {
    return false;
  }
  {
    const std::lock_guard<std::mutex> global_lock(g_mutex);
    if (g_active != nullptr) {
      return false;
    }
  }

  ID3D11Device* device = nullptr;
  if (swapchain->GetDevice(__uuidof(ID3D11Device),
                           reinterpret_cast<void**>(&device)) != S_OK ||
      device == nullptr) {
    return false;
  }
  ID3D11DeviceContext* context = nullptr;
  device->GetImmediateContext(&context);
  device->Release();
  if (context == nullptr) {
    return false;
  }

  if (!present_hook_.install(swapchain, kPresentIndex,
                             reinterpret_cast<void*>(&HookPresent))) {
    context->Release();
    return false;
  }
  if (!resize_hook_.install(swapchain, kResizeBuffersIndex,
                            reinterpret_cast<void*>(&HookResizeBuffers))) {
    present_hook_.uninstall();
    context->Release();
    return false;
  }

  context_ = context;  // Owned ref from GetImmediateContext.
  {
    const std::lock_guard<std::mutex> global_lock(g_mutex);
    g_active = this;
  }
  installed_flag_ = true;
  return true;
}

bool Observer::uninstall() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!installed_flag_) {
    return true;
  }
  {
    const std::lock_guard<std::mutex> global_lock(g_mutex);
    if (g_active == this) {
      g_active = nullptr;
    }
  }
  const bool present_ok = present_hook_.uninstall();
  const bool resize_ok = resize_hook_.uninstall();
  if (context_ != nullptr) {
    context_->Release();
    context_ = nullptr;
  }
  installed_flag_ = false;
  return present_ok && resize_ok;
}

bool Observer::installed() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return installed_flag_;
}

std::vector<PresentCall> Observer::presentCalls() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return present_calls_;
}

std::vector<ResizeCall> Observer::resizeCalls() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return resize_calls_;
}

void* Observer::originalPresent() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return present_hook_.original();
}

void* Observer::originalResizeBuffers() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return resize_hook_.original();
}

int Observer::hookPresent(IDXGISwapChain* self, unsigned int sync_interval,
                          unsigned int flags) {
  PresentCall call;
  call.sync_interval = sync_interval;
  call.flags = flags;
  call.thread_id = GetCurrentThreadId();
  call.timestamp = Clock::now();

  PresentFn original = nullptr;
  ID3D11DeviceContext* context = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    original = reinterpret_cast<PresentFn>(present_hook_.original());
    context = context_;
  }
  HRESULT hr = E_UNEXPECTED;
  if (original != nullptr && context != nullptr) {
    const StateGuard before(context);
    hr = original(self, sync_interval, flags);
    call.state_preserved = before.verifyUnchanged();
  }
  call.result = static_cast<int>(hr);
  const std::lock_guard<std::mutex> lock(mutex_);
  present_calls_.push_back(call);
  return static_cast<int>(hr);
}

int Observer::hookResizeBuffers(IDXGISwapChain* self,
                                unsigned int buffer_count, unsigned int width,
                                unsigned int height, unsigned int format,
                                unsigned int flags) {
  ResizeCall call;
  call.buffer_count = buffer_count;
  call.width = width;
  call.height = height;
  call.format = format;
  call.flags = flags;
  call.thread_id = GetCurrentThreadId();
  call.timestamp = Clock::now();

  ResizeBuffersFn original = nullptr;
  ID3D11DeviceContext* context = nullptr;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    original = reinterpret_cast<ResizeBuffersFn>(resize_hook_.original());
    context = context_;
  }
  HRESULT hr = E_UNEXPECTED;
  if (original != nullptr && context != nullptr) {
    const StateGuard before(context);
    hr = original(self, buffer_count, width, height,
                 static_cast<DXGI_FORMAT>(format), flags);
    call.state_preserved = before.verifyUnchanged();
  }
  call.result = static_cast<int>(hr);
  const std::lock_guard<std::mutex> lock(mutex_);
  resize_calls_.push_back(call);
  return static_cast<int>(hr);
}

}  // namespace mecvr::render
