#include <d3d11.h>

#include <cmath>
#include <iostream>

#include "ik/full_body_ik.h"
#include "render/body_overlay.h"

namespace {

bool Check(bool value, const char* label) {
  if (!value) std::cerr << "FAIL: " << label << '\n';
  return value;
}

mecvr::ik::HumanoidPoseFrame Pose() {
  mecvr::ik::FullBodyInput input;
  input.head.valid = true;
  input.head.position = {0.0, 1.70, 0.0};
  input.left_hand.pose.valid = true;
  input.left_hand.pose.position = {-0.38, 1.25, -0.30};
  input.left_hand.trigger = 1.0f;
  input.right_hand.pose.valid = true;
  input.right_hand.pose.position = {0.38, 1.25, -0.30};
  input.right_hand.trigger = 1.0f;
  input.floor_origin = {0.0, 0.0, 0.0};
  input.delta_seconds = 1.0 / 90.0;
  mecvr::ik::FullBodyAnimator animator;
  return animator.update(input);
}

}  // namespace

int main() {
  bool ok = true;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL level{};
  const HRESULT device_hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
      D3D11_SDK_VERSION, &device, &level, &context);
  ok &= Check(SUCCEEDED(device_hr), "WARP D3D11 device");
  if (!ok) return 1;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 256;
  desc.Height = 256;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* target_texture = nullptr;
  ID3D11RenderTargetView* target = nullptr;
  ok &= Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target_texture)),
              "overlay target texture");
  ok &= Check(target_texture != nullptr &&
                  SUCCEEDED(device->CreateRenderTargetView(target_texture,
                                                            nullptr, &target)),
              "overlay target view");
  if (!ok) {
    if (target != nullptr) target->Release();
    if (target_texture != nullptr) target_texture->Release();
    context->Release();
    device->Release();
    return 1;
  }
  const float clear[4] = {0, 0, 0, 1};
  context->ClearRenderTargetView(target, clear);
  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<float>(desc.Width);
  viewport.Height = static_cast<float>(desc.Height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context->RSSetViewports(1, &viewport);
  mecvr::render::BodyOverlay overlay;
  ok &= Check(overlay.render(context, target, Pose()),
              "overlay renders on WARP");
  context->Flush();

  D3D11_TEXTURE2D_DESC readback = desc;
  readback.Usage = D3D11_USAGE_STAGING;
  readback.BindFlags = 0;
  readback.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging = nullptr;
  ok &= Check(SUCCEEDED(device->CreateTexture2D(&readback, nullptr, &staging)),
              "overlay readback texture");
  if (staging != nullptr) {
    context->CopyResource(staging, target_texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ok &= Check(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0,
                                        &mapped)),
                "overlay readback map");
    if (mapped.pData != nullptr) {
      const auto* pixels = static_cast<const unsigned char*>(mapped.pData);
      bool changed = false;
      for (UINT y = 0; y < desc.Height && !changed; ++y) {
        for (UINT x = 0; x < desc.Width; ++x) {
          const auto* pixel = pixels + y * mapped.RowPitch + x * 4;
          if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
            changed = true;
            break;
          }
        }
      }
      ok &= Check(changed, "overlay changes render target pixels");
      context->Unmap(staging, 0);
    }
    staging->Release();
  }
  target->Release();
  target_texture->Release();
  context->Release();
  device->Release();
  std::cout << (ok ? "BODY_OVERLAY_RENDER_TEST: PASS\n"
                   : "BODY_OVERLAY_RENDER_TEST: FAIL\n");
  return ok ? 0 : 1;
}
