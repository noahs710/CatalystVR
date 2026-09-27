// M3a vtable slot prover (headless, never touches a live game).
// Resolves ID3D11DeviceContext vtable indices empirically: installs one
// generic counting detour at a single slot, issues exactly one API call,
// and records whether it fired; scans slots 0..127 per API. On x64 the
// caller cleans the stack, so a zero-arg detour is safe at any slot,
// and skipped calls are harmless headless (results ignored, Map/Unmap
// balanced explicitly). Asserts OMSetRenderTargets==33 (E1 ground truth,
// validates the method) plus uniqueness; prints every resolved index.
#include <cstdio>
#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "render/vtable_hook.h"

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

constexpr int kMaxSlot = 128;

volatile LONG g_fired = 0;
volatile LONG g_device_fired = 0;
// Set when an Unmap proof iteration skipped the real Unmap (resource left
// mapped); main() balances it once after the scan (hook uninstalled).
volatile LONG g_unmap_pending = 0;
volatile LONG g_map_pending = 0;  // Map-scan balance skipped: mapped.

// Generic detour: records the hit, skips the real call. Safe at any
// slot on x64 (caller stack cleanup; test ignores results).
void STDMETHODCALLTYPE GenericFire() { g_fired = 1; }
HRESULT STDMETHODCALLTYPE GenericDeviceFire(ID3D11Device*, UINT,
                                            ID3D11DeviceContext**) {
  g_device_fired = 1;
  return E_FAIL;
}

using CallFn = void (*)(ID3D11DeviceContext* ctx, void* userdata);

struct Fixture {
  ID3D11Buffer* dyn_cb = nullptr;      // DYNAMIC, for Map/Unmap.
  ID3D11Buffer* def_cb = nullptr;      // DEFAULT, for UpdateSubresource.
  ID3D11Buffer* args = nullptr;        // INDIRECT_ARGS, for indirect draws.
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11CommandList* list = nullptr;  // Empty, for ExecuteCommandList.
  bool has_shaders = false;
  bool has_list = false;
};

// One API call per prover function (results deliberately ignored).
void CallVSSetCB(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->VSSetConstantBuffers(0, 1, &fx->dyn_cb);
}
void CallPSSetCB(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->PSSetConstantBuffers(0, 1, &fx->dyn_cb);
}
void CallGSSetCB(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->GSSetConstantBuffers(0, 1, &fx->dyn_cb);
}
void CallCSSetCB(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->CSSetConstantBuffers(0, 1, &fx->dyn_cb);
}
void CallVSSetShader(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->VSSetShader(fx->vs, nullptr, 0);
}
void CallPSSetShader(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->PSSetShader(fx->ps, nullptr, 0);
}
void CallDrawIndexed(ID3D11DeviceContext* ctx, void*) {
  ctx->DrawIndexed(3, 0, 0);
}
void CallDraw(ID3D11DeviceContext* ctx, void*) { ctx->Draw(3, 0); }
void CallDrawIndexedInstanced(ID3D11DeviceContext* ctx, void*) {
  ctx->DrawIndexedInstanced(3, 1, 0, 0, 0);
}
void CallDrawInstanced(ID3D11DeviceContext* ctx, void*) {
  ctx->DrawInstanced(3, 1, 0, 0);
}

void CallDrawAuto(ID3D11DeviceContext* ctx, void*) { ctx->DrawAuto(); }

void CallDrawIndexedInstancedIndirect(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->DrawIndexedInstancedIndirect(fx->args, 0);
}

void CallDrawInstancedIndirect(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->DrawInstancedIndirect(fx->args, 0);
}
void CallUpdateSubresource(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  unsigned char zeros[256] = {};
  ctx->UpdateSubresource(fx->def_cb, 0, nullptr, zeros, 0, 0);
}
void CallMap(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  g_fired = 0;
  const HRESULT hr = ctx->Map(fx->dyn_cb, 0, D3D11_MAP_WRITE_DISCARD, 0,
                              &mapped);
  const LONG map_fired = g_fired;  // Report ONLY the Map call below.
  if (map_fired == 0 && SUCCEEDED(hr)) {
    g_fired = 0;
    ctx->Unmap(fx->dyn_cb, 0);  // Balance; may hit the detour itself.
    if (g_fired != 0) g_map_pending = 1;  // Balance skipped: mapped.
  }
  g_fired = map_fired;
}
void CallUnmap(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  g_fired = 0;
  const HRESULT hr =
      ctx->Map(fx->dyn_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  // Report ONLY the Unmap call: reset before every early return.
  if (g_fired != 0) {
    g_fired = 0;
    return;  // Map itself is hooked this iteration: skip.
  }
  if (FAILED(hr)) {
    g_fired = 0;
    return;  // Left mapped by an earlier iteration: skip.
  }
  g_fired = 0;
  ctx->Unmap(fx->dyn_cb, 0);
  // If the detour fired, the real Unmap was skipped and the resource is
  // left mapped; main() balances it after the scan. Do NOT Unmap here:
  // the hook is still installed and would skip again.
  if (g_fired != 0) g_unmap_pending = 1;
}
void CallRSSetViewports(ID3D11DeviceContext* ctx, void*) {
  D3D11_VIEWPORT vp{};
  vp.Width = 64.0f;
  vp.Height = 64.0f;
  vp.MaxDepth = 1.0f;
  ctx->RSSetViewports(1, &vp);
}
void CallRSSetState(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->RSSetState(fx->rs);
}
void CallOMSetRT(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->OMSetRenderTargets(1, &fx->rtv, nullptr);
}
void CallClearRTV(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  const float c[4] = {0, 0, 0, 0};
  ctx->ClearRenderTargetView(fx->rtv, c);
}
void CallIATopo(ID3D11DeviceContext* ctx, void*) {
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
void CallExecute(ID3D11DeviceContext* ctx, void* userdata) {
  auto* fx = static_cast<Fixture*>(userdata);
  ctx->ExecuteCommandList(fx->list, FALSE);
}

// Scans slots for one API; returns the firing slot or -1. Asserts
// uniqueness (exactly one slot fires across the whole range).
int ProveSlot(ID3D11DeviceContext* ctx, const char* name, CallFn call,
              Fixture* fx) {
  int found = -1;
  int hits = 0;
  for (int slot = 0; slot < kMaxSlot; ++slot) {
    mecvr::render::VtableHook hook;
    if (!hook.install(ctx, static_cast<std::size_t>(slot),
                      reinterpret_cast<void*>(&GenericFire))) {
      continue;  // Protection change failed; not our slot anyway.
    }
    g_fired = 0;
    call(ctx, fx);
    hook.uninstall();
    if (g_fired != 0) {
      found = slot;
      ++hits;
    }
  }
  char msg[160];
  std::snprintf(msg, sizeof(msg), "%s resolves to exactly one slot", name);
  Check(hits == 1, msg);
  std::printf("SLOT %s = %d\n", name, found);
  return found;
}

int ProveDeferredContextSlot(ID3D11Device* device) {
  int found = -1;
  int hits = 0;
  for (int slot = 0; slot < kMaxSlot; ++slot) {
    mecvr::render::VtableHook hook;
    if (!hook.install(device, static_cast<std::size_t>(slot),
                      reinterpret_cast<void*>(&GenericDeviceFire))) {
      continue;
    }
    g_device_fired = 0;
    ID3D11DeviceContext* deferred = nullptr;
    const HRESULT hr = device->CreateDeferredContext(0, &deferred);
    hook.uninstall();
    if (g_device_fired != 0) {
      if (found < 0) found = slot;
      ++hits;
    } else if (SUCCEEDED(hr) && deferred != nullptr) {
      deferred->Release();
    }
  }
  Check(found == 27, "CreateDeferredContext direct dispatch resolves to slot 27");
  std::printf("SLOT CreateDeferredContext = %d\n", found);
  return found;
}

using D3DCompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*,
                                      const void*, void*, const char*,
                                      const char*, UINT, UINT, void**, void**);

}  // namespace

int main() {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  const D3D_DRIVER_TYPE drivers[2] = {D3D_DRIVER_TYPE_HARDWARE,
                                      D3D_DRIVER_TYPE_WARP};
  HRESULT hr = E_FAIL;
  for (int i = 0; i < 2 && FAILED(hr); ++i) {
    hr = D3D11CreateDevice(nullptr, drivers[i], nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &device, nullptr, &context);
  }
  Check(SUCCEEDED(hr), "D3D11 device created");
  if (FAILED(hr)) return 1;

  const int deferred_slot = ProveDeferredContextSlot(device);
  Check(deferred_slot == 27, "CreateDeferredContext == 27");

  Fixture fx;
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 256;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device->CreateBuffer(&bd, nullptr, &fx.dyn_cb);
  Check(SUCCEEDED(hr), "dynamic CB created");
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.CPUAccessFlags = 0;
  hr = device->CreateBuffer(&bd, nullptr, &fx.def_cb);
  Check(SUCCEEDED(hr), "default CB created");
  D3D11_BUFFER_DESC args_desc{};
  args_desc.ByteWidth = 16;
  args_desc.Usage = D3D11_USAGE_DEFAULT;
  args_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
  hr = device->CreateBuffer(&args_desc, nullptr, &fx.args);
  Check(SUCCEEDED(hr), "indirect args buffer created");

  D3D11_TEXTURE2D_DESC td{};
  td.Width = 64;
  td.Height = 64;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* tex = nullptr;
  hr = device->CreateTexture2D(&td, nullptr, &tex);
  Check(SUCCEEDED(hr), "RT texture created");
  hr = device->CreateRenderTargetView(tex, nullptr, &fx.rtv);
  Check(SUCCEEDED(hr), "RTV created");
  if (tex != nullptr) tex->Release();

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_BACK;
  hr = device->CreateRasterizerState(&rd, &fx.rs);
  Check(SUCCEEDED(hr), "rasterizer state created");

  // Shaders via the inbox system compiler (test-only dependency).
  const HMODULE compiler = LoadLibraryW(L"D3DCompiler_47.dll");
  if (compiler != nullptr) {
    auto compile = reinterpret_cast<D3DCompileFn>(
        GetProcAddress(compiler, "D3DCompile"));
    if (compile != nullptr) {
      const char* vs_src =
          "float4 main(float4 p : POSITION) : SV_POSITION { return p; }";
      const char* ps_src =
          "float4 main() : SV_Target { return float4(1,0,0,1); }";
      void *vs_blob = nullptr, *ps_blob = nullptr, *err = nullptr;
      if (SUCCEEDED(compile(vs_src, strlen(vs_src), nullptr, nullptr, nullptr,
                            "main", "vs_4_0", 0, 0, &vs_blob, &err))) {
        auto* blob = static_cast<ID3DBlob*>(vs_blob);
        if (SUCCEEDED(device->CreateVertexShader(
                blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                &fx.vs))) {
          fx.has_shaders = true;
        }
        blob->Release();
      } else if (err != nullptr) {
        static_cast<ID3DBlob*>(err)->Release();
      }
      err = nullptr;
      if (fx.has_shaders &&
          SUCCEEDED(compile(ps_src, strlen(ps_src), nullptr, nullptr, nullptr,
                            "main", "ps_4_0", 0, 0, &ps_blob, &err))) {
        auto* blob = static_cast<ID3DBlob*>(ps_blob);
        ID3D11PixelShader* ps = nullptr;
        if (SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(),
                                                blob->GetBufferSize(), nullptr,
                                                &ps))) {
          fx.ps = ps;
        } else {
          fx.has_shaders = false;
        }
        blob->Release();
      } else {
        fx.has_shaders = false;
        if (err != nullptr) static_cast<ID3DBlob*>(err)->Release();
      }
    }
  }
  Check(fx.has_shaders, "test shaders compiled (D3DCompiler_47)");

  ID3D11DeviceContext* deferred = nullptr;
  if (SUCCEEDED(device->CreateDeferredContext(0, &deferred)) &&
      deferred != nullptr) {
    if (SUCCEEDED(deferred->FinishCommandList(FALSE, &fx.list)) &&
        fx.list != nullptr) {
      fx.has_list = true;
    }
    deferred->Release();
  }
  Check(fx.has_list, "empty command list finished");

  const int vs_cb = ProveSlot(context, "VSSetConstantBuffers", CallVSSetCB, &fx);
  const int ps_cb = ProveSlot(context, "PSSetConstantBuffers", CallPSSetCB, &fx);
  const int gs_cb = ProveSlot(context, "GSSetConstantBuffers", CallGSSetCB, &fx);
  const int cs_cb = ProveSlot(context, "CSSetConstantBuffers", CallCSSetCB, &fx);
  Check(vs_cb == 7, "VSSetConstantBuffers == 7");
  Check(ps_cb == 16, "PSSetConstantBuffers == 16");
  Check(gs_cb == 22, "GSSetConstantBuffers == 22");
  Check(cs_cb == 71, "CSSetConstantBuffers == 71");
  int vs_set = -1, ps_set = -1;
  if (fx.has_shaders) {
    vs_set = ProveSlot(context, "VSSetShader", CallVSSetShader, &fx);
    ps_set = ProveSlot(context, "PSSetShader", CallPSSetShader, &fx);
    Check(vs_set == 11, "VSSetShader == 11");
    Check(ps_set == 9, "PSSetShader == 9");
  }
  const int di = ProveSlot(context, "DrawIndexed", CallDrawIndexed, &fx);
  const int dr = ProveSlot(context, "Draw", CallDraw, &fx);
  const int dii =
      ProveSlot(context, "DrawIndexedInstanced", CallDrawIndexedInstanced, &fx);
  const int dri = ProveSlot(context, "DrawInstanced", CallDrawInstanced, &fx);
  const int da = ProveSlot(context, "DrawAuto", CallDrawAuto, &fx);
  const int diii = ProveSlot(context, "DrawIndexedInstancedIndirect",
                             CallDrawIndexedInstancedIndirect, &fx);
  const int diii2 = ProveSlot(context, "DrawInstancedIndirect",
                              CallDrawInstancedIndirect, &fx);
  Check(di == 12, "DrawIndexed == 12");
  Check(dr == 13, "Draw == 13");
  Check(dii == 20, "DrawIndexedInstanced == 20");
  Check(dri == 21, "DrawInstanced == 21");
  Check(da == 38, "DrawAuto == 38");
  Check(diii == 39, "DrawIndexedInstancedIndirect == 39");
  Check(diii2 == 40, "DrawInstancedIndirect == 40");
  const int upd =
      ProveSlot(context, "UpdateSubresource", CallUpdateSubresource, &fx);
  Check(upd == 48, "UpdateSubresource == 48");
  const int map = ProveSlot(context, "Map", CallMap, &fx);
  if (g_map_pending != 0) {
    context->Unmap(fx.dyn_cb, 0);  // Hook uninstalled: real Unmap.
    g_map_pending = 0;
  }
  const int unmap = ProveSlot(context, "Unmap", CallUnmap, &fx);
  Check(map == 14, "Map == 14");
  Check(unmap == 15, "Unmap == 15");
  if (g_unmap_pending != 0) {
    context->Unmap(fx.dyn_cb, 0);  // Hook uninstalled: real Unmap.
    g_unmap_pending = 0;
  }
  const int rsvp = ProveSlot(context, "RSSetViewports", CallRSSetViewports, &fx);
  const int rss = ProveSlot(context, "RSSetState", CallRSSetState, &fx);
  Check(rsvp == 44, "RSSetViewports == 44");
  Check(rss == 43, "RSSetState == 43");
  const int om_rt = ProveSlot(context, "OMSetRenderTargets", CallOMSetRT, &fx);
  Check(om_rt == 33, "OMSetRenderTargets == 33 (E1 ground truth)");
  const int crtv = ProveSlot(context, "ClearRenderTargetView", CallClearRTV, &fx);
  Check(crtv == 50, "ClearRenderTargetView == 50");
  const int topo = ProveSlot(context, "IASetPrimitiveTopology", CallIATopo, &fx);
  Check(topo == 24, "IASetPrimitiveTopology == 24");
  if (fx.has_list) {
    const int execl =
        ProveSlot(context, "ExecuteCommandList", CallExecute, &fx);
    Check(execl == 58, "ExecuteCommandList == 58");
  }

  if (fx.list != nullptr) fx.list->Release();
  if (fx.dyn_cb != nullptr) fx.dyn_cb->Release();
  if (fx.def_cb != nullptr) fx.def_cb->Release();
  if (fx.args != nullptr) fx.args->Release();
  if (fx.vs != nullptr) fx.vs->Release();
  if (fx.ps != nullptr) fx.ps->Release();
  if (fx.rs != nullptr) fx.rs->Release();
  if (fx.rtv != nullptr) fx.rtv->Release();
  context->Release();
  device->Release();
  if (compiler != nullptr) FreeLibrary(compiler);

  if (g_failures == 0) {
    std::printf("M3A_SLOTS_ALL_PASS\n");
    return 0;
  }
  std::printf("M3A_SLOTS_FAILURES=%d\n", g_failures);
  return 1;
}
