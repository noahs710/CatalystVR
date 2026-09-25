// Synthetic M0C1 observer test (T3A). Creates a real D3D11 device +
// swapchain on a hidden window, installs the vtable-patch observer, issues
// Present/ResizeBuffers calls, and asserts interception data, state
// preservation, and clean unhook. Never touches a live game process.
#include <cstdio>

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include "render/observer.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
  if (condition) {
    std::printf("PASS: %s\n", message);
  } else {
    std::printf("FAIL: %s\n", message);
    ++g_failures;
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void* VtableSlot(IDXGISwapChain* swapchain, std::size_t index) {
  void** vtable = *reinterpret_cast<void***>(swapchain);
  return vtable[index];
}

}  // namespace

int main() {
  constexpr UINT kWidth = 800;
  constexpr UINT kHeight = 600;
  const DWORD main_thread = GetCurrentThreadId();
  std::printf("main thread id: %lu\n", static_cast<unsigned long>(main_thread));

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW klass = {};
  klass.lpfnWndProc = &WndProc;
  klass.hInstance = instance;
  klass.lpszClassName = L"mecvr-t3a-hidden";
  if (RegisterClassW(&klass) == 0) {
    std::printf("FAIL: RegisterClassW failed (%lu)\n", GetLastError());
    return 1;
  }
  const HWND hwnd = CreateWindowExW(0, klass.lpszClassName, L"mecvr-t3a",
                                    WS_POPUP, 0, 0, static_cast<int>(kWidth),
                                    static_cast<int>(kHeight), nullptr, nullptr,
                                    instance, nullptr);
  Check(hwnd != nullptr, "hidden window created (never shown)");
  if (hwnd == nullptr) {
    return 1;
  }

  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferCount = 2;
  desc.BufferDesc.Width = kWidth;
  desc.BufferDesc.Height = kHeight;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.OutputWindow = hwnd;
  desc.SampleDesc.Count = 1;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swapchain = nullptr;
  const D3D_DRIVER_TYPE drivers[2] = {D3D_DRIVER_TYPE_HARDWARE,
                                      D3D_DRIVER_TYPE_WARP};
  HRESULT hr = E_FAIL;
  for (int i = 0; i < 2 && FAILED(hr); ++i) {
    hr = D3D11CreateDeviceAndSwapChain(
        nullptr, drivers[i], nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc,
        &swapchain, &device, nullptr, &context);
  }
  Check(SUCCEEDED(hr), "D3D11 device + swapchain created");
  if (FAILED(hr)) {
    DestroyWindow(hwnd);
    return 1;
  }

  // Bind nontrivial state so the guard has something to verify: RTV on the
  // back buffer, a depth-stencil view, explicit rasterizer/blend/
  // depth-stencil states, and one viewport.
  ID3D11Texture2D* back_buffer = nullptr;
  hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&back_buffer));
  Check(SUCCEEDED(hr), "back buffer acquired");
  ID3D11RenderTargetView* rtv = nullptr;
  hr = device->CreateRenderTargetView(back_buffer, nullptr, &rtv);
  Check(SUCCEEDED(hr), "render-target view created");
  back_buffer->Release();

  D3D11_TEXTURE2D_DESC depth_desc = {};
  depth_desc.Width = kWidth;
  depth_desc.Height = kHeight;
  depth_desc.MipLevels = 1;
  depth_desc.ArraySize = 1;
  depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  depth_desc.SampleDesc.Count = 1;
  depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D* depth_texture = nullptr;
  hr = device->CreateTexture2D(&depth_desc, nullptr, &depth_texture);
  Check(SUCCEEDED(hr), "depth texture created");
  ID3D11DepthStencilView* dsv = nullptr;
  hr = device->CreateDepthStencilView(depth_texture, nullptr, &dsv);
  Check(SUCCEEDED(hr), "depth-stencil view created");
  depth_texture->Release();

  D3D11_RASTERIZER_DESC rs_desc = {};
  rs_desc.FillMode = D3D11_FILL_SOLID;
  rs_desc.CullMode = D3D11_CULL_BACK;
  rs_desc.DepthClipEnable = TRUE;
  ID3D11RasterizerState* rs_state = nullptr;
  hr = device->CreateRasterizerState(&rs_desc, &rs_state);
  Check(SUCCEEDED(hr), "rasterizer state created");
  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.RenderTarget[0].BlendEnable = FALSE;
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState* blend_state = nullptr;
  hr = device->CreateBlendState(&blend_desc, &blend_state);
  Check(SUCCEEDED(hr), "blend state created");
  D3D11_DEPTH_STENCIL_DESC ds_desc = {};
  ds_desc.DepthEnable = TRUE;
  ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  ds_desc.DepthFunc = D3D11_COMPARISON_LESS;
  ID3D11DepthStencilState* ds_state = nullptr;
  hr = device->CreateDepthStencilState(&ds_desc, &ds_state);
  Check(SUCCEEDED(hr), "depth-stencil state created");

  context->OMSetRenderTargets(1, &rtv, dsv);
  context->RSSetState(rs_state);
  const float blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  context->OMSetBlendState(blend_state, blend_factor, 0xffffffff);
  context->OMSetDepthStencilState(ds_state, 7);
  D3D11_VIEWPORT viewport = {};
  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = static_cast<float>(kWidth);
  viewport.Height = static_cast<float>(kHeight);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context->RSSetViewports(1, &viewport);

  mecvr::render::Observer observer;
  Check(observer.install(swapchain), "observer hook installed");
  Check(observer.installed(), "observer reports installed");
  Check(!observer.install(swapchain), "second install fails closed");

  hr = swapchain->Present(0, 0);
  Check(SUCCEEDED(hr), "Present(0,0) #1 returned success");

  // DXGI requires every back-buffer reference released before
  // ResizeBuffers, including the context binding: unbind, release the RTV,
  // resize, then rebind a fresh RTV.
  ID3D11RenderTargetView* null_rtv = nullptr;
  context->OMSetRenderTargets(1, &null_rtv, nullptr);
  rtv->Release();
  rtv = nullptr;
  hr = swapchain->ResizeBuffers(2, kWidth, kHeight,
                                DXGI_FORMAT_R8G8B8A8_UNORM, 0);
  Check(SUCCEEDED(hr), "ResizeBuffers returned success");

  ID3D11Texture2D* back_buffer2 = nullptr;
  hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&back_buffer2));
  Check(SUCCEEDED(hr), "post-resize back buffer acquired");
  ID3D11RenderTargetView* rtv2 = nullptr;
  hr = device->CreateRenderTargetView(back_buffer2, nullptr, &rtv2);
  Check(SUCCEEDED(hr), "post-resize render-target view created");
  back_buffer2->Release();
  context->OMSetRenderTargets(1, &rtv2, dsv);
  hr = swapchain->Present(0, 0);
  Check(SUCCEEDED(hr), "Present(0,0) #2 returned success");

  const std::vector<mecvr::render::PresentCall> presents =
      observer.presentCalls();
  const std::vector<mecvr::render::ResizeCall> resizes =
      observer.resizeCalls();
  Check(presents.size() == 2, "two Present interceptions recorded");
  Check(resizes.size() == 1, "one ResizeBuffers interception recorded");
  if (presents.size() == 2) {
    Check(presents[0].sync_interval == 0 && presents[0].flags == 0,
          "Present #1 parameters recorded");
    Check(presents[1].sync_interval == 0 && presents[1].flags == 0,
          "Present #2 parameters recorded");
    Check(presents[0].thread_id == main_thread &&
              presents[1].thread_id == main_thread,
          "Present calling thread id matches");
    Check(presents[0].result == S_OK && presents[1].result == S_OK,
          "Present HRESULTs recorded");
    Check(presents[0].state_preserved && presents[1].state_preserved,
          "D3D11 state preserved across Present hooks");
    Check(presents[1].timestamp >= presents[0].timestamp,
          "Present timestamps monotonic");
  }
  if (resizes.size() == 1) {
    Check(resizes[0].buffer_count == 2 && resizes[0].width == kWidth &&
              resizes[0].height == kHeight &&
              resizes[0].format == DXGI_FORMAT_R8G8B8A8_UNORM &&
              resizes[0].flags == 0,
          "ResizeBuffers parameters recorded");
    Check(resizes[0].thread_id == main_thread,
          "ResizeBuffers calling thread id matches");
    Check(resizes[0].result == S_OK, "ResizeBuffers HRESULT recorded");
    Check(resizes[0].state_preserved,
          "D3D11 state preserved across ResizeBuffers hook");
  }

  void* orig_present = observer.originalPresent();
  void* orig_resize = observer.originalResizeBuffers();
  Check(orig_present != nullptr && orig_resize != nullptr,
        "original function pointers captured");
  Check(observer.uninstall(), "observer unhooked cleanly");
  Check(!observer.installed(), "observer reports uninstalled");
  Check(VtableSlot(swapchain, 8) == orig_present,
        "Present vtable slot restored to original");
  Check(VtableSlot(swapchain, 13) == orig_resize,
        "ResizeBuffers vtable slot restored to original");

  hr = swapchain->Present(0, 0);
  Check(SUCCEEDED(hr), "post-unhook Present works");
  Check(observer.presentCalls().size() == 2,
        "no interceptions recorded after unhook");

  if (rtv != nullptr) {
    rtv->Release();
  }
  rtv2->Release();
  dsv->Release();
  rs_state->Release();
  blend_state->Release();
  ds_state->Release();
  context->Release();
  device->Release();
  swapchain->Release();
  DestroyWindow(hwnd);

  if (g_failures == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("%d CHECK(S) FAILED\n", g_failures);
  return 1;
}
