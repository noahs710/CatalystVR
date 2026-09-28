#include "render/live_capture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <d3dcompiler.h>
#include <memory>
#include <thread>

#include <dxgi1_2.h>

#include "render/m2b_mono.h"

namespace mecvr::render {
namespace {

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

MonoFramePtr MakeSquareFrame(const MonoFramePtr& source) {
  if (source == nullptr || source->width == 0 || source->height == 0 ||
      source->pixels_rgba.empty()) {
    return nullptr;
  }
  const std::uint32_t side = (std::min)(source->width, source->height);
  const std::uint32_t offset_x = (source->width - side) / 2;
  const std::uint32_t offset_y = (source->height - side) / 2;
  auto square = std::make_shared<MonoFrame>();
  square->sequence = source->sequence;
  square->capture_time_ns = source->capture_time_ns;
  square->width = side;
  square->height = side;
  square->pixels_rgba.resize(static_cast<std::size_t>(side) * side * 4u);
  for (std::uint32_t y = 0; y < side; ++y) {
    const auto* src = source->pixels_rgba.data() +
                      (static_cast<std::size_t>(y + offset_y) * source->width +
                       offset_x) * 4u;
    auto* dst = square->pixels_rgba.data() + static_cast<std::size_t>(y) * side * 4u;
    std::memcpy(dst, src, static_cast<std::size_t>(side) * 4u);
  }
  return square;
}

// CPU readback is the dominant cost of the desktop-to-XR bridge. Catalyst
// can present well above the headset cadence (especially on ultrawide
// displays); reading every Present starves the game. One bounded capture per
// 60 Hz tick is enough for the compositor to reproject between updates and
// keeps the render hook out of the game's frame-critical path.
constexpr std::int64_t kMinCaptureIntervalNs = 16666666;
constexpr std::uint32_t kMaxReadbackWidth = 1920;
constexpr std::uint32_t kMaxReadbackHeight = 1080;

template <typename T>
void Release(T** value) {
  if (value != nullptr && *value != nullptr) {
    (*value)->Release();
    *value = nullptr;
  }
}

bool Compile(const char* source, const char* entry, const char* target,
             ID3DBlob** blob) {
  if (blob == nullptr) return false;
  *blob = nullptr;
  ID3DBlob* errors = nullptr;
  const HRESULT hr = D3DCompile(
      source, std::strlen(source), "mecvr_capture_scale.hlsl", nullptr,
      nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob,
      &errors);
  Release(&errors);
  return SUCCEEDED(hr) && *blob != nullptr;
}

constexpr char kScaleVertexShader[] = R"(
struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(uint vertex_id : SV_VertexID) {
  float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0),
                          float2(3.0, -1.0) };
  VSOut output;
  output.position = float4(positions[vertex_id], 0.0, 1.0);
  output.uv = float2((positions[vertex_id].x + 1.0) * 0.5,
                     1.0 - (positions[vertex_id].y + 1.0) * 0.5);
  return output;
})";

constexpr char kScalePixelShader[] = R"(
Texture2D source_texture : register(t0);
SamplerState source_sampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0)
    : SV_TARGET {
  return source_texture.SampleLevel(source_sampler, uv, 0.0);
})";

struct ScaleStateGuard {
  explicit ScaleStateGuard(ID3D11DeviceContext* context) : context(context) {
    context->OMGetRenderTargets(1, &rtv, &dsv);
    context->RSGetState(&rasterizer);
    context->OMGetBlendState(&blend, blend_factor, &sample_mask);
    context->OMGetDepthStencilState(&depth_stencil, &stencil_ref);
    context->RSGetViewports(&viewport_count, &viewport);
    context->IAGetInputLayout(&input_layout);
    context->IAGetPrimitiveTopology(&topology);
    context->VSGetShader(&vertex_shader, nullptr, nullptr);
    context->PSGetShader(&pixel_shader, nullptr, nullptr);
    context->PSGetShaderResources(0, 1, &source_view);
    context->PSGetSamplers(0, 1, &sampler);
  }

  ~ScaleStateGuard() {
    context->OMSetRenderTargets(1, &rtv, dsv);
    context->RSSetState(rasterizer);
    context->OMSetBlendState(blend, blend_factor, sample_mask);
    context->OMSetDepthStencilState(depth_stencil, stencil_ref);
    if (viewport_count != 0) context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(input_layout);
    context->IASetPrimitiveTopology(topology);
    context->VSSetShader(vertex_shader, nullptr, 0);
    context->PSSetShader(pixel_shader, nullptr, 0);
    context->PSSetShaderResources(0, 1, &source_view);
    context->PSSetSamplers(0, 1, &sampler);
    Release(&rtv);
    Release(&dsv);
    Release(&rasterizer);
    Release(&blend);
    Release(&depth_stencil);
    Release(&input_layout);
    Release(&vertex_shader);
    Release(&pixel_shader);
    Release(&source_view);
    Release(&sampler);
  }

  ID3D11DeviceContext* context = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11RasterizerState* rasterizer = nullptr;
  ID3D11BlendState* blend = nullptr;
  ID3D11DepthStencilState* depth_stencil = nullptr;
  ID3D11InputLayout* input_layout = nullptr;
  ID3D11VertexShader* vertex_shader = nullptr;
  ID3D11PixelShader* pixel_shader = nullptr;
  ID3D11ShaderResourceView* source_view = nullptr;
  ID3D11SamplerState* sampler = nullptr;
  FLOAT blend_factor[4] = {};
  UINT sample_mask = 0;
  UINT stencil_ref = 0;
  UINT viewport_count = 1;
  D3D11_VIEWPORT viewport{};
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
};

}  // namespace

LiveCapture::~LiveCapture() {
  reset();
  // A consumer lease is the generation's lifetime token. Shutdown may wait
  // for that lease, but must never invalidate handles while it is held.
  while (gpu_device_ != nullptr && !releaseGpuResources()) {
    std::this_thread::yield();
  }
}

void LiveCapture::reset() {
  releaseResources();
  releaseGpuResources();
  staging_width_ = 0;
  staging_height_ = 0;
  staging_format_ = DXGI_FORMAT_UNKNOWN;
  last_stereo_capture_ns_ = {};
  stereo_pixels_[0].clear();
  stereo_pixels_[1].clear();
  stereo_width_ = {};
  stereo_height_ = {};
  stereo_epoch_ = 0;
  stereo_pose_sequence_ = 0;
  stereo_left_capture_ns_ = 0;
  stereo_left_valid_ = false;
}

bool LiveCapture::releaseGpuResources() {
  if (gpu_mailbox_ != nullptr && !gpu_mailbox_->reset()) return false;

  std::lock_guard<std::mutex> lock(gpu_registration_mutex_);
  gpu_registration_ = {};
  for (std::size_t i = 0; i < gpu_textures_.size(); ++i) {
    Release(&gpu_mutexes_[i]);
    Release(&gpu_textures_[i]);
    if (gpu_handles_[i] != nullptr) {
      CloseHandle(static_cast<HANDLE>(gpu_handles_[i]));
      gpu_handles_[i] = nullptr;
    }
  }
  Release(&gpu_device_);
  gpu_width_ = 0;
  gpu_height_ = 0;
  gpu_format_ = DXGI_FORMAT_UNKNOWN;
  gpu_next_slot_ = 0;
  return true;
}

bool LiveCapture::ensureGpuResources(
    ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source) {
  if (device == nullptr || source.Width == 0 || source.Height == 0 ||
      source.SampleDesc.Count != 1 || source.ArraySize != 1 ||
      source.MipLevels != 1) {
    return false;
  }
  if (gpu_device_ == device && gpu_width_ == source.Width &&
      gpu_height_ == source.Height && gpu_format_ == source.Format &&
      gpu_textures_[0] != nullptr) {
    return true;
  }

  std::array<ID3D11Texture2D*, 3> textures{};
  std::array<IDXGIKeyedMutex*, 3> mutexes{};
  std::array<void*, 3> handles{};
  AdapterLuid luid{};
  bool created = false;
  do {
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(&dxgi_device))) ||
        dxgi_device == nullptr ||
        FAILED(dxgi_device->GetAdapter(&adapter)) || adapter == nullptr) {
      Release(&adapter);
      Release(&dxgi_device);
      break;
    }
    DXGI_ADAPTER_DESC adapter_desc{};
    const HRESULT adapter_hr = adapter->GetDesc(&adapter_desc);
    Release(&adapter);
    Release(&dxgi_device);
    if (FAILED(adapter_hr)) break;
    luid.low = adapter_desc.AdapterLuid.LowPart;
    luid.high = adapter_desc.AdapterLuid.HighPart;

    D3D11_TEXTURE2D_DESC shared{};
    shared.Width = source.Width;
    shared.Height = source.Height;
    shared.MipLevels = 1;
    shared.ArraySize = 1;
    shared.Format = source.Format;
    shared.SampleDesc.Count = 1;
    shared.Usage = D3D11_USAGE_DEFAULT;
    shared.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    shared.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                       D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    created = true;
    for (std::size_t i = 0; i < textures.size(); ++i) {
      IDXGIResource1* resource = nullptr;
      if (FAILED(device->CreateTexture2D(&shared, nullptr, &textures[i])) ||
          textures[i] == nullptr ||
          FAILED(textures[i]->QueryInterface(
              __uuidof(IDXGIKeyedMutex),
              reinterpret_cast<void**>(&mutexes[i]))) ||
          mutexes[i] == nullptr ||
          FAILED(textures[i]->QueryInterface(
              __uuidof(IDXGIResource1),
              reinterpret_cast<void**>(&resource))) ||
          resource == nullptr) {
        Release(&resource);
        created = false;
        break;
      }
      HANDLE handle = nullptr;
      const HRESULT handle_hr = resource->CreateSharedHandle(
          nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
          nullptr, &handle);
      Release(&resource);
      if (FAILED(handle_hr) || handle == nullptr) {
        created = false;
        break;
      }
      handles[i] = handle;
    }
  } while (false);

  if (!created || gpu_mailbox_ == nullptr || !gpu_mailbox_->reset()) {
    for (std::size_t i = 0; i < textures.size(); ++i) {
      Release(&mutexes[i]);
      Release(&textures[i]);
      if (handles[i] != nullptr) CloseHandle(static_cast<HANDLE>(handles[i]));
    }
    return false;
  }

  std::lock_guard<std::mutex> lock(gpu_registration_mutex_);
  gpu_registration_ = {};
  for (std::size_t i = 0; i < gpu_textures_.size(); ++i) {
    Release(&gpu_mutexes_[i]);
    Release(&gpu_textures_[i]);
    if (gpu_handles_[i] != nullptr)
      CloseHandle(static_cast<HANDLE>(gpu_handles_[i]));
    gpu_textures_[i] = textures[i];
    gpu_mutexes_[i] = mutexes[i];
    gpu_handles_[i] = handles[i];
  }
  Release(&gpu_device_);
  gpu_device_ = device;
  gpu_device_->AddRef();
  gpu_width_ = source.Width;
  gpu_height_ = source.Height;
  gpu_format_ = source.Format;
  gpu_next_slot_ = 0;
  ++gpu_generation_;
  if (gpu_generation_ == 0) ++gpu_generation_;
  gpu_registration_.luid = luid;
  gpu_registration_.width = gpu_width_;
  gpu_registration_.height = gpu_height_;
  gpu_registration_.format = static_cast<std::uint32_t>(gpu_format_);
  gpu_registration_.generation = gpu_generation_;
  gpu_registration_.slot_count =
      static_cast<std::uint32_t>(gpu_textures_.size());
  for (std::size_t i = 0; i < gpu_handles_.size(); ++i) {
    gpu_registration_.handles[i] = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(gpu_handles_[i]));
  }
  return true;
}

bool LiveCapture::sharedRegistration(
    SharedCaptureRegistration* registration) const {
  if (registration == nullptr) return false;
  std::lock_guard<std::mutex> lock(gpu_registration_mutex_);
  if (!IsValidSharedCaptureRegistration(gpu_registration_)) return false;
  *registration = gpu_registration_;
  return true;
}

void LiveCapture::setGpuConsumerReady(bool ready) {
  gpu_consumer_ready_.store(ready, std::memory_order_release);
}

bool LiveCapture::captureGpu(IDXGISwapChain* swapchain) {
  if (swapchain == nullptr || gpu_mailbox_ == nullptr) {
    return false;
  }

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11Texture2D* backbuffer = nullptr;
  bool published = false;
  do {
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device),
                                    reinterpret_cast<void**>(&device))) ||
        device == nullptr) {
      break;
    }
    device->GetImmediateContext(&context);
    if (context == nullptr ||
        FAILED(swapchain->GetBuffer(
            0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backbuffer))) ||
        backbuffer == nullptr) {
      break;
    }
    D3D11_TEXTURE2D_DESC source{};
    backbuffer->GetDesc(&source);
    if (!ensureGpuResources(device, source)) break;
    // Resource creation publishes the registration first. The XR worker opens
    // the handles asynchronously, then enables production on a later Present.
    if (!gpu_consumer_ready_.load(std::memory_order_acquire)) break;

    auto lease = gpu_mailbox_->tryReserve();
    if (!lease) break;

    std::uint32_t selected = static_cast<std::uint32_t>(gpu_textures_.size());
    for (std::size_t attempt = 0; attempt < gpu_textures_.size(); ++attempt) {
      const std::uint32_t slot =
          (gpu_next_slot_ + static_cast<std::uint32_t>(attempt)) %
          static_cast<std::uint32_t>(gpu_textures_.size());
      if (gpu_mutexes_[slot] != nullptr &&
          gpu_mutexes_[slot]->AcquireSync(0, 0) == S_OK) {
        selected = slot;
        break;
      }
    }
    if (selected >= gpu_textures_.size()) break;

    context->CopyResource(gpu_textures_[selected], backbuffer);
    if (FAILED(gpu_mutexes_[selected]->ReleaseSync(1))) break;

    ++gpu_sequence_;
    if (gpu_sequence_ == 0) ++gpu_sequence_;
    const std::uint64_t now = static_cast<std::uint64_t>(NowNs());
    SharedCaptureFrame& frame = lease.payload();
    frame.slot = selected;
    frame.generation = gpu_generation_;
    frame.sequence = gpu_sequence_;
    frame.width = gpu_width_;
    frame.height = gpu_height_;
    frame.format = static_cast<std::uint32_t>(gpu_format_);
    frame.capture_time_ns = now;
    published = lease.publish({gpu_generation_, gpu_sequence_});
    if (published) gpu_next_slot_ = (selected + 1) % gpu_textures_.size();
  } while (false);

  Release(&backbuffer);
  Release(&context);
  Release(&device);
  return published;
}

void LiveCapture::releaseResources() {
  Release(&staging_);
  Release(&scale_source_view_);
  Release(&scale_source_);
  Release(&scale_target_view_);
  Release(&scale_target_);
  Release(&scale_vertex_shader_);
  Release(&scale_pixel_shader_);
  Release(&scale_sampler_);
  staging_width_ = 0;
  staging_height_ = 0;
  staging_format_ = DXGI_FORMAT_UNKNOWN;
  scale_source_width_ = 0;
  scale_source_height_ = 0;
  scale_width_ = 0;
  scale_height_ = 0;
  scale_format_ = DXGI_FORMAT_UNKNOWN;
}

bool LiveCapture::ensureScaleResources(ID3D11Device* device,
                                       const D3D11_TEXTURE2D_DESC& source,
                                       std::uint32_t width,
                                       std::uint32_t height) {
  if (device == nullptr || width == 0 || height == 0) return false;
  if (scale_source_ != nullptr && scale_source_width_ == source.Width &&
      scale_source_height_ == source.Height && scale_width_ == width &&
      scale_height_ == height && scale_format_ == source.Format) {
    return true;
  }
  Release(&scale_source_view_);
  Release(&scale_source_);
  Release(&scale_target_view_);
  Release(&scale_target_);
  Release(&scale_vertex_shader_);
  Release(&scale_pixel_shader_);
  Release(&scale_sampler_);

  D3D11_TEXTURE2D_DESC source_copy = source;
  source_copy.MipLevels = 1;
  source_copy.ArraySize = 1;
  source_copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  source_copy.MiscFlags = 0;
  source_copy.CPUAccessFlags = 0;
  if (FAILED(device->CreateTexture2D(&source_copy, nullptr, &scale_source_)))
    return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC source_view{};
  source_view.Format = source.Format;
  source_view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  source_view.Texture2D.MipLevels = 1;
  if (FAILED(device->CreateShaderResourceView(scale_source_, &source_view,
                                               &scale_source_view_)))
    return false;

  D3D11_TEXTURE2D_DESC target = source;
  target.Width = width;
  target.Height = height;
  target.MipLevels = 1;
  target.ArraySize = 1;
  target.BindFlags = D3D11_BIND_RENDER_TARGET;
  target.MiscFlags = 0;
  target.CPUAccessFlags = 0;
  if (FAILED(device->CreateTexture2D(&target, nullptr, &scale_target_)))
    return false;
  D3D11_RENDER_TARGET_VIEW_DESC target_view{};
  target_view.Format = source.Format;
  target_view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  target_view.Texture2D.MipSlice = 0;
  if (FAILED(device->CreateRenderTargetView(scale_target_, &target_view,
                                             &scale_target_view_)))
    return false;

  ID3DBlob* vertex_blob = nullptr;
  ID3DBlob* pixel_blob = nullptr;
  if (!Compile(kScaleVertexShader, "main", "vs_5_0", &vertex_blob) ||
      !Compile(kScalePixelShader, "main", "ps_5_0", &pixel_blob)) {
    Release(&vertex_blob);
    Release(&pixel_blob);
    return false;
  }
  HRESULT hr = device->CreateVertexShader(
      vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(), nullptr,
      &scale_vertex_shader_);
  if (SUCCEEDED(hr)) {
    hr = device->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                   pixel_blob->GetBufferSize(), nullptr,
                                   &scale_pixel_shader_);
  }
  Release(&vertex_blob);
  Release(&pixel_blob);
  if (FAILED(hr)) return false;

  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MinLOD = 0.0f;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sampler, &scale_sampler_)))
    return false;

  scale_source_width_ = source.Width;
  scale_source_height_ = source.Height;
  scale_width_ = width;
  scale_height_ = height;
  scale_format_ = source.Format;
  return true;
}

MonoFramePtr LiveCapture::copyFrame(IDXGISwapChain* swapchain,
                                    std::int64_t now) {
  if (swapchain == nullptr) return nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11Texture2D* backbuffer = nullptr;
  MonoFramePtr frame;
  do {
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device),
                                   reinterpret_cast<void**>(&device))) ||
        device == nullptr) {
      break;
    }
    device->GetImmediateContext(&context);
    if (context == nullptr ||
        FAILED(swapchain->GetBuffer(
            0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backbuffer))) ||
        backbuffer == nullptr) {
      break;
    }

    D3D11_TEXTURE2D_DESC source{};
    backbuffer->GetDesc(&source);
    if (source.Width == 0 || source.Height == 0 || source.Width > 8192 ||
        source.Height > 8192 || source.SampleDesc.Count != 1 ||
        (source.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         source.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         source.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         source.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
      break;
    }
    const double scale = (std::min)(
        1.0, (std::min)(static_cast<double>(kMaxReadbackWidth) / source.Width,
                        static_cast<double>(kMaxReadbackHeight) / source.Height));
    const std::uint32_t readback_width = (std::max)(
        1u, static_cast<std::uint32_t>(source.Width * scale));
    const std::uint32_t readback_height = (std::max)(
        1u, static_cast<std::uint32_t>(source.Height * scale));
    const bool scaled = readback_width != source.Width ||
                        readback_height != source.Height;
    if (scaled &&
        !ensureScaleResources(device, source, readback_width, readback_height)) {
      break;
    }
    if (staging_ == nullptr || staging_width_ != readback_width ||
        staging_height_ != readback_height || staging_format_ != source.Format) {
      if (staging_ != nullptr) {
        staging_->Release();
        staging_ = nullptr;
      }
      D3D11_TEXTURE2D_DESC staging{};
      staging.Width = readback_width;
      staging.Height = readback_height;
      staging.MipLevels = 1;
      staging.ArraySize = 1;
      staging.Format = source.Format;
      staging.SampleDesc.Count = 1;
      staging.Usage = D3D11_USAGE_STAGING;
      staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(device->CreateTexture2D(&staging, nullptr, &staging_)) ||
          staging_ == nullptr) {
        break;
      }
      staging_width_ = readback_width;
      staging_height_ = readback_height;
      staging_format_ = source.Format;
    }

    if (scaled) {
      context->CopyResource(scale_source_, backbuffer);
      ScaleStateGuard state(context);
      D3D11_VIEWPORT viewport{};
      viewport.Width = static_cast<float>(readback_width);
      viewport.Height = static_cast<float>(readback_height);
      viewport.MaxDepth = 1.0f;
      context->OMSetRenderTargets(1, &scale_target_view_, nullptr);
      context->RSSetViewports(1, &viewport);
      context->IASetInputLayout(nullptr);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context->VSSetShader(scale_vertex_shader_, nullptr, 0);
      context->PSSetShader(scale_pixel_shader_, nullptr, 0);
      context->PSSetShaderResources(0, 1, &scale_source_view_);
      context->PSSetSamplers(0, 1, &scale_sampler_);
      context->Draw(3, 0);
      context->CopyResource(staging_, scale_target_);
    } else {
      context->CopyResource(staging_, backbuffer);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped)) ||
        mapped.pData == nullptr) {
      break;
    }
    auto mutable_frame = std::make_shared<MonoFrame>();
    mutable_frame->sequence = static_cast<std::uint64_t>(now);
    mutable_frame->capture_time_ns = now;
    mutable_frame->width = readback_width;
    mutable_frame->height = readback_height;
    const std::size_t row = static_cast<std::size_t>(readback_width) * 4u;
    mutable_frame->pixels_rgba.resize(row * readback_height);
    const auto* source_bytes = static_cast<const std::uint8_t*>(mapped.pData);
    for (UINT y = 0; y < readback_height; ++y) {
      std::memcpy(mutable_frame->pixels_rgba.data() + y * row,
                  source_bytes + static_cast<std::size_t>(y) * mapped.RowPitch,
                  row);
    }
    context->Unmap(staging_, 0);
    frame = std::move(mutable_frame);
  } while (false);

  if (backbuffer != nullptr) backbuffer->Release();
  if (context != nullptr) context->Release();
  if (device != nullptr) device->Release();
  return frame;
}

void LiveCapture::capture(IDXGISwapChain* swapchain) {
  const std::int64_t now = NowNs();
  if (swapchain == nullptr || mailbox_ == nullptr ||
      now - last_capture_ns_ < kMinCaptureIntervalNs ||
      mailbox_->depth() > 0) {
    return;
  }

  // Every CPU frame entering an XR mailbox is normalized at this boundary.
  // The desktop swapchain may be 16:9, ultrawide, or otherwise arbitrary;
  // XR presentation must never inherit that aspect ratio as its scene image.
  MonoFramePtr frame = MakeSquareFrame(copyFrame(swapchain, now));
  if (frame != nullptr && mailbox_->tryPublish(frame)) last_capture_ns_ = now;
}

void LiveCapture::captureStereo(IDXGISwapChain* swapchain, std::uint32_t eye,
                                std::uint64_t epoch,
                                std::uint64_t pose_sequence) {
  if (stereo_mailbox_ == nullptr || eye >= 2 || epoch == 0 ||
      pose_sequence == 0) {
    capture(swapchain);
    return;
  }
  const std::int64_t now = NowNs();
  if (swapchain == nullptr ||
      now - last_stereo_capture_ns_[eye] < kMinCaptureIntervalNs) {
    return;
  }
  // The XR projection path is square/near-square by contract. Crop the
  // desktop backbuffer once at the transport boundary so the OpenXR eye
  // texture is never a 16:9 desktop image that later gets zoomed into the
  // headset's view rectangle.
  MonoFramePtr frame = MakeSquareFrame(copyFrame(swapchain, now));
  if (frame == nullptr) return;
  last_stereo_capture_ns_[eye] = now;
  if (mailbox_ != nullptr && mailbox_->depth() == 0) {
    mailbox_->tryPublish(frame);  // Mono remains the safe fallback.
  }
  stereo_pixels_[eye] = frame->pixels_rgba;
  stereo_width_[eye] = frame->width;
  stereo_height_[eye] = frame->height;
  if (eye == 0) {
    stereo_epoch_ = epoch;
    stereo_pose_sequence_ = pose_sequence;
    stereo_left_capture_ns_ = now;
    stereo_left_valid_ = true;
  } else if (stereo_left_valid_ && stereo_epoch_ == epoch &&
             stereo_pose_sequence_ == pose_sequence &&
             now - stereo_left_capture_ns_ < 100000000) {
    auto stereo = std::make_shared<StereoFrame>();
    stereo->epoch = epoch;
    stereo->pose_sequence = pose_sequence;
    stereo->width[0] = stereo_width_[0];
    stereo->height[0] = stereo_height_[0];
    stereo->width[1] = stereo_width_[1];
    stereo->height[1] = stereo_height_[1];
    stereo->pixels_rgba[0] = stereo_pixels_[0];
    stereo->pixels_rgba[1] = stereo_pixels_[1];
    stereo->capture_time_ns = now;
    if (stereo_mailbox_->tryPublish(stereo)) stereo_left_valid_ = false;
  }
  last_capture_ns_ = now;
}

}  // namespace mecvr::render
