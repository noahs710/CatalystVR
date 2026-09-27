#pragma once

#include <d3d11.h>

#include "ik/full_body_ik.h"
#include "render/body_overlay_geometry.h"

namespace mecvr::render {

// A deliberately self-contained, opt-in first-person body renderer. It is
// independent of Catalyst's private animation ABI and only draws after the
// game's scene has completed. The overlay is a bridge toward mod-owned visible
// IK; it never writes Catalyst buffers or replaces its skeleton.
class BodyOverlay {
 public:
  BodyOverlay() = default;
  ~BodyOverlay();
  BodyOverlay(const BodyOverlay&) = delete;
  BodyOverlay& operator=(const BodyOverlay&) = delete;

  bool render(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
              const mecvr::ik::HumanoidPoseFrame& pose,
              float eye_offset_x = 0.0f);
  void reset();

 private:
  bool ensureResources(ID3D11Device* device);
  void releaseResources();

  ID3D11Device* device_ = nullptr;
  ID3D11VertexShader* vertex_shader_ = nullptr;
  ID3D11PixelShader* pixel_shader_ = nullptr;
  ID3D11InputLayout* input_layout_ = nullptr;
  ID3D11Buffer* vertex_buffer_ = nullptr;
  ID3D11BlendState* blend_state_ = nullptr;
  ID3D11RasterizerState* rasterizer_state_ = nullptr;
};

}  // namespace mecvr::render
