#pragma once

// Synthetic DX11 observer (M0C1, T3A). Records IDXGISwapChain::Present and
// ResizeBuffers interceptions and verifies D3D11 immediate-context state
// preservation across each hook. Synthetic scope only: at most one Observer
// is installed at a time, and no live game process is ever involved.
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "render/vtable_hook.h"

struct IDXGISwapChain;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11RasterizerState;
struct ID3D11RenderTargetView;

namespace mecvr::render {

using Clock = std::chrono::steady_clock;

struct PresentCall {
  unsigned int sync_interval = 0;
  unsigned int flags = 0;
  std::uint32_t thread_id = 0;
  Clock::time_point timestamp{};
  int result = 0;  // HRESULT value; kept as int to avoid windows.h here.
  bool state_preserved = false;
};

struct ResizeCall {
  unsigned int buffer_count = 0;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int format = 0;  // DXGI_FORMAT numeric value.
  unsigned int flags = 0;
  std::uint32_t thread_id = 0;
  Clock::time_point timestamp{};
  int result = 0;  // HRESULT value; kept as int to avoid windows.h here.
  bool state_preserved = false;
};

// Snapshots a documented subset of immediate-context state and verifies it
// is unchanged across a hook invocation. Subset:
//   - OM render targets: first render-target slot + depth-stencil view.
//   - RS viewports: bound count + first viewport (all six fields).
//   - RS rasterizer state, OM blend state (+ factor, sample mask),
//     OM depth-stencil state (+ stencil ref); compared by object identity,
//     null included.
class StateGuard {
 public:
  explicit StateGuard(ID3D11DeviceContext* context);
  ~StateGuard();

  StateGuard(const StateGuard&) = delete;
  StateGuard& operator=(const StateGuard&) = delete;

  // Re-queries live context state and compares it to the snapshot.
  // Returns true when everything in the subset matches; otherwise returns
  // false and records a short reason retrievable via mismatch().
  bool verifyUnchanged() const;
  const std::string& mismatch() const { return mismatch_; }

 private:
  ID3D11DeviceContext* context_;
  ID3D11RenderTargetView* rtv_ = nullptr;
  ID3D11DepthStencilView* dsv_ = nullptr;
  ID3D11RasterizerState* rasterizer_ = nullptr;
  ID3D11BlendState* blend_ = nullptr;
  ID3D11DepthStencilState* depth_stencil_ = nullptr;
  float blend_factor_[4] = {};
  unsigned int sample_mask_ = 0;
  unsigned int stencil_ref_ = 0;
  unsigned int viewport_count_ = 0;
  float viewport_x_ = 0.0f;
  float viewport_y_ = 0.0f;
  float viewport_width_ = 0.0f;
  float viewport_height_ = 0.0f;
  float viewport_min_depth_ = 0.0f;
  float viewport_max_depth_ = 0.0f;
  mutable std::string mismatch_;
};

class Observer {
 public:
  Observer() = default;
  ~Observer() { uninstall(); }

  Observer(const Observer&) = delete;
  Observer& operator=(const Observer&) = delete;

  // Installs Present/ResizeBuffers hooks on the swapchain's vtable and grabs
  // the immediate context from the swapchain's device. The vtable is shared
  // per COM class, so while installed every swapchain using that vtable is
  // observed. Fails when another Observer is already installed.
  bool install(IDXGISwapChain* swapchain);
  bool uninstall();
  bool installed() const;

  std::vector<PresentCall> presentCalls() const;
  std::vector<ResizeCall> resizeCalls() const;

  // Original function pointers captured at install; exposed for tests to
  // prove clean unhook (vtable slot == original after uninstall).
  void* originalPresent() const;
  void* originalResizeBuffers() const;

  // Hook-internal entry points invoked by the vtable detours. Public only
  // because the detours are free functions; not for general use.
  int hookPresent(IDXGISwapChain* self, unsigned int sync_interval,
                  unsigned int flags);
  int hookResizeBuffers(IDXGISwapChain* self, unsigned int buffer_count,
                        unsigned int width, unsigned int height,
                        unsigned int format, unsigned int flags);

 private:
  mutable std::mutex mutex_;
  bool installed_flag_ = false;
  ID3D11DeviceContext* context_ = nullptr;  // Owned ref while installed.
  VtableHook present_hook_;
  VtableHook resize_hook_;
  std::vector<PresentCall> present_calls_;
  std::vector<ResizeCall> resize_calls_;
};

}  // namespace mecvr::render
