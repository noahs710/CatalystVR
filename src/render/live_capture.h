#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>

#include "openxr/mailbox.h"
#include "openxr/stereo_mailbox.h"
#include "render/shared_capture_mailbox.h"

namespace mecvr::render {

using SharedGpuMailbox = SharedCaptureMailbox<SharedCaptureFrame, 3>;

// Copies the game's final backbuffer into a bounded CPU frame mailbox. The
// capture is deliberately render-thread local and never waits for XR; the
// XR worker owns pacing and consumes the newest completed frame.
class LiveCapture final {
 public:
  explicit LiveCapture(openxr::FrameMailbox* mailbox,
                       openxr::StereoMailbox* stereo_mailbox = nullptr,
                       SharedGpuMailbox* gpu_mailbox = nullptr)
      : mailbox_(mailbox),
        stereo_mailbox_(stereo_mailbox),
        gpu_mailbox_(gpu_mailbox) {}
  ~LiveCapture();

  LiveCapture(const LiveCapture&) = delete;
  LiveCapture& operator=(const LiveCapture&) = delete;

  void capture(IDXGISwapChain* swapchain);
  void captureStereo(IDXGISwapChain* swapchain, std::uint32_t eye,
                     std::uint64_t epoch, std::uint64_t pose_sequence);
  bool captureGpu(IDXGISwapChain* swapchain);
  bool sharedRegistration(SharedCaptureRegistration* registration) const;
  void setGpuConsumerReady(bool ready);
  void reset();

 private:
  MonoFramePtr copyFrame(IDXGISwapChain* swapchain, std::int64_t now);
  void releaseResources();
  bool releaseGpuResources();
  bool ensureGpuResources(ID3D11Device* device,
                          const D3D11_TEXTURE2D_DESC& source);
  bool ensureScaleResources(ID3D11Device* device,
                            const D3D11_TEXTURE2D_DESC& source,
                            std::uint32_t width, std::uint32_t height);

  openxr::FrameMailbox* mailbox_ = nullptr;
  openxr::StereoMailbox* stereo_mailbox_ = nullptr;
  SharedGpuMailbox* gpu_mailbox_ = nullptr;
  std::array<ID3D11Texture2D*, 3> gpu_textures_{};
  std::array<IDXGIKeyedMutex*, 3> gpu_mutexes_{};
  std::array<void*, 3> gpu_handles_{};
  ID3D11Device* gpu_device_ = nullptr;
  std::uint32_t gpu_width_ = 0;
  std::uint32_t gpu_height_ = 0;
  DXGI_FORMAT gpu_format_ = DXGI_FORMAT_UNKNOWN;
  std::uint64_t gpu_generation_ = 0;
  std::uint64_t gpu_sequence_ = 0;
  std::uint32_t gpu_next_slot_ = 0;
  std::atomic<bool> gpu_consumer_ready_{false};
  mutable std::mutex gpu_registration_mutex_;
  SharedCaptureRegistration gpu_registration_{};
  ID3D11Texture2D* staging_ = nullptr;
  std::uint32_t staging_width_ = 0;
  std::uint32_t staging_height_ = 0;
  DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture2D* scale_source_ = nullptr;
  ID3D11ShaderResourceView* scale_source_view_ = nullptr;
  ID3D11Texture2D* scale_target_ = nullptr;
  ID3D11RenderTargetView* scale_target_view_ = nullptr;
  ID3D11VertexShader* scale_vertex_shader_ = nullptr;
  ID3D11PixelShader* scale_pixel_shader_ = nullptr;
  ID3D11SamplerState* scale_sampler_ = nullptr;
  std::uint32_t scale_source_width_ = 0;
  std::uint32_t scale_source_height_ = 0;
  std::uint32_t scale_width_ = 0;
  std::uint32_t scale_height_ = 0;
  DXGI_FORMAT scale_format_ = DXGI_FORMAT_UNKNOWN;
  std::int64_t last_capture_ns_ = 0;
  std::array<std::vector<std::uint8_t>, 2> stereo_pixels_;
  std::array<std::uint32_t, 2> stereo_width_{};
  std::array<std::uint32_t, 2> stereo_height_{};
  std::uint64_t stereo_epoch_ = 0;
  std::uint64_t stereo_pose_sequence_ = 0;
  std::int64_t stereo_left_capture_ns_ = 0;
  bool stereo_left_valid_ = false;
};

}  // namespace mecvr::render
