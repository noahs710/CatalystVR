#include "render/body_overlay.h"

#include <d3dcompiler.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mecvr::render {
namespace {

constexpr char kVertexShader[] = R"(
struct VSIn { float3 position : POSITION; float4 color : COLOR; };
struct VSOut { float4 position : SV_POSITION; float4 color : COLOR; };
VSOut main(VSIn input) {
  VSOut output;
  output.position = float4(input.position, 1.0);
  const float depth_lift = 0.94 + (1.0 - saturate(input.position.z)) * 0.12;
  output.color = float4(input.color.rgb * depth_lift, input.color.a);
  return output;
})";

constexpr char kPixelShader[] = R"(
struct PSIn { float4 position : SV_POSITION; float4 color : COLOR; };
float4 main(PSIn input) : SV_TARGET { return input.color; }
)";

void Release(IUnknown* value) {
  if (value != nullptr) value->Release();
}

bool Compile(const char* source, const char* entry, const char* target,
             ID3DBlob** blob) {
  if (blob == nullptr) return false;
  *blob = nullptr;
  ID3DBlob* errors = nullptr;
  const HRESULT hr = D3DCompile(
      source, std::strlen(source), "mecvr_body_overlay.hlsl", nullptr, nullptr,
      entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
  Release(errors);
  return SUCCEEDED(hr) && *blob != nullptr;
}

struct PipelineStateGuard {
  explicit PipelineStateGuard(ID3D11DeviceContext* context) : context(context) {
    context->OMGetRenderTargets(1, &rtv, &dsv);
    context->RSGetState(&rasterizer);
    context->OMGetBlendState(&blend, blend_factor, &sample_mask);
    context->IAGetInputLayout(&input_layout);
    context->IAGetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    context->IAGetPrimitiveTopology(&topology);
    vs_class_count = static_cast<UINT>(std::size(vs_classes));
    ps_class_count = static_cast<UINT>(std::size(ps_classes));
    context->VSGetShader(&vertex_shader, vs_classes, &vs_class_count);
    context->PSGetShader(&pixel_shader, ps_classes, &ps_class_count);
  }

  ~PipelineStateGuard() {
    context->OMSetRenderTargets(1, &rtv, dsv);
    context->RSSetState(rasterizer);
    context->OMSetBlendState(blend, blend_factor, sample_mask);
    context->IASetInputLayout(input_layout);
    context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    context->IASetPrimitiveTopology(topology);
    context->VSSetShader(vertex_shader, vs_classes, vs_class_count);
    context->PSSetShader(pixel_shader, ps_classes, ps_class_count);
    Release(rtv);
    Release(dsv);
    Release(rasterizer);
    Release(blend);
    Release(input_layout);
    Release(vertex_buffer);
    Release(vertex_shader);
    Release(pixel_shader);
    for (UINT i = 0; i < vs_class_count; ++i) Release(vs_classes[i]);
    for (UINT i = 0; i < ps_class_count; ++i) Release(ps_classes[i]);
  }

  ID3D11DeviceContext* context = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11RasterizerState* rasterizer = nullptr;
  ID3D11BlendState* blend = nullptr;
  ID3D11InputLayout* input_layout = nullptr;
  ID3D11Buffer* vertex_buffer = nullptr;
  ID3D11VertexShader* vertex_shader = nullptr;
  ID3D11PixelShader* pixel_shader = nullptr;
  ID3D11ClassInstance* vs_classes[16] = {};
  ID3D11ClassInstance* ps_classes[16] = {};
  UINT vs_class_count = 0;
  UINT ps_class_count = 0;
  FLOAT blend_factor[4] = {};
  UINT sample_mask = 0;
  UINT stride = 0;
  UINT offset = 0;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
};

bool Finite(float value) { return std::isfinite(value); }

}  // namespace

BodyOverlay::~BodyOverlay() { releaseResources(); }

void BodyOverlay::releaseResources() {
  Release(vertex_buffer_);
  Release(input_layout_);
  Release(vertex_shader_);
  Release(pixel_shader_);
  Release(blend_state_);
  Release(rasterizer_state_);
  Release(device_);
  vertex_buffer_ = nullptr;
  input_layout_ = nullptr;
  vertex_shader_ = nullptr;
  pixel_shader_ = nullptr;
  blend_state_ = nullptr;
  rasterizer_state_ = nullptr;
  device_ = nullptr;
}

void BodyOverlay::reset() { releaseResources(); }

bool BodyOverlay::ensureResources(ID3D11Device* device) {
  if (device == nullptr) return false;
  if (device_ == device && vertex_buffer_ != nullptr) return true;
  releaseResources();
  device_ = device;
  device_->AddRef();

  ID3DBlob* vs_blob = nullptr;
  ID3DBlob* ps_blob = nullptr;
  if (!Compile(kVertexShader, "main", "vs_4_0", &vs_blob) ||
      !Compile(kPixelShader, "main", "ps_4_0", &ps_blob)) {
    Release(vs_blob);
    Release(ps_blob);
    releaseResources();
    return false;
  }
  HRESULT hr = device_->CreateVertexShader(vs_blob->GetBufferPointer(),
                                           vs_blob->GetBufferSize(), nullptr,
                                           &vertex_shader_);
  if (SUCCEEDED(hr)) {
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device_->CreateInputLayout(
        elements, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        &input_layout_);
  }
  if (SUCCEEDED(hr)) {
    hr = device_->CreatePixelShader(ps_blob->GetBufferPointer(),
                                    ps_blob->GetBufferSize(), nullptr,
                                    &pixel_shader_);
  }
  Release(vs_blob);
  Release(ps_blob);
  if (FAILED(hr)) {
    releaseResources();
    return false;
  }

  D3D11_BUFFER_DESC buffer{};
  buffer.ByteWidth = static_cast<UINT>(sizeof(BodyOverlayVertex) * 512);
  buffer.Usage = D3D11_USAGE_DYNAMIC;
  buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device_->CreateBuffer(&buffer, nullptr, &vertex_buffer_))) {
    releaseResources();
    return false;
  }

  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0].BlendEnable = TRUE;
  blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device_->CreateBlendState(&blend, &blend_state_))) {
    releaseResources();
    return false;
  }

  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  if (FAILED(device_->CreateRasterizerState(&raster, &rasterizer_state_))) {
    releaseResources();
    return false;
  }
  return true;
}

bool BodyOverlay::render(ID3D11DeviceContext* context,
                           ID3D11RenderTargetView* target,
                           const mecvr::ik::HumanoidPoseFrame& pose,
                           const BodyOverlayView* view) {
  if (context == nullptr || target == nullptr || !pose.valid) return false;
  ID3D11Device* device = nullptr;
  context->GetDevice(&device);
  const bool ready = ensureResources(device);
  Release(device);
  if (!ready) return false;

  std::array<BodyOverlayVertex, 512> vertices{};
    const std::size_t count =
       BuildBodyOverlayGeometry(pose, vertices.data(), vertices.size(), view);
  if (count == 0) return false;
  for (std::size_t i = 0; i < count; ++i) {
    if (!Finite(vertices[i].x) || !Finite(vertices[i].y) ||
        !Finite(vertices[i].depth) || vertices[i].depth < 0.0f ||
        vertices[i].depth > 1.0f)
      return false;
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(vertex_buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0,
                          &mapped)))
    return false;
  std::memcpy(mapped.pData, vertices.data(), sizeof(BodyOverlayVertex) * count);
  context->Unmap(vertex_buffer_, 0);

  // The overlay is injected into the game's immediate context immediately
  // before Present. Preserve every pipeline binding we touch so the capture
  // path and any late Present-side middleware observe the game's state.
  PipelineStateGuard state(context);
  ID3D11RenderTargetView* rtvs[] = {target};
  const FLOAT blend_factor[4] = {0, 0, 0, 0};
  UINT stride = sizeof(BodyOverlayVertex);
  UINT offset = 0;
  context->OMSetRenderTargets(1, rtvs, state.dsv);
  context->OMSetBlendState(blend_state_, blend_factor, 0xffffffffu);
  context->RSSetState(rasterizer_state_);
  context->IASetInputLayout(input_layout_);
  context->IASetVertexBuffers(0, 1, &vertex_buffer_, &stride, &offset);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex_shader_, nullptr, 0);
  context->PSSetShader(pixel_shader_, nullptr, 0);
  context->Draw(static_cast<UINT>(count), 0);
  return true;
}

}  // namespace mecvr::render
