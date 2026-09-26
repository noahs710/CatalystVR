#include "openxr/test/xr_scene.h"

// M1C test-scene implementation (plan T9). See xr_scene.h for the contract.

#pragma warning(push, 3)
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#pragma warning(pop)

#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

namespace mecvr::openxr::scene {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265358979323846f;

// Clear colors (float) per eye. Bytes below are round(c*255).
constexpr float kClearLeft[4] = {0.55f, 0.07f, 0.10f, 1.0f};    // reddish
constexpr float kClearRight[4] = {0.08f, 0.12f, 0.62f, 1.0f};   // bluish
// Quad base colors (float) per eye, brightness-modulated by head yaw.
constexpr float kQuadLeft[3] = {1.0f, 0.55f, 0.10f};    // orange
constexpr float kQuadRight[3] = {0.10f, 0.85f, 1.0f};   // cyan

std::uint8_t FloatToByte(float c) {
  const long v = std::lround(c * 255.0f);  // NOLINT: bounded [0,255] below
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<std::uint8_t>(v);
}

float QuadBrightness(float yaw_radians) {
  return 0.8f + 0.2f * std::cos(yaw_radians);
}

bool PixelsNear(const std::uint8_t a[4], const std::uint8_t b[4]) {
  for (int i = 0; i < 4; ++i) {
    const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
    if (d < -2 || d > 2) return false;
  }
  return true;
}

}  // namespace

const char* SessionStateName(SessionState state) {
  switch (state) {
    case SessionState::kIdle:
      return "IDLE";
    case SessionState::kReady:
      return "READY";
    case SessionState::kSynchronized:
      return "SYNCHRONIZED";
    case SessionState::kVisible:
      return "VISIBLE";
    case SessionState::kFocused:
      return "FOCUSED";
    case SessionState::kStopping:
      return "STOPPING";
    case SessionState::kLossPending:
      return "LOSS_PENDING";
    case SessionState::kExiting:
      return "EXITING";
    case SessionState::kUnknown:
    default:
      return "UNKNOWN";
  }
}

float YawFromQuaternion(const XrQuaternionf& q) {
  const float s = 2.0f * (q.w * q.y + q.x * q.z);
  const float c = 1.0f - 2.0f * (q.y * q.y + q.x * q.x);
  return std::atan2(s, c);
}

std::vector<PhaseSpec> DefaultMockScript() {
  return std::vector<PhaseSpec>{
      {SessionState::kIdle, 5, false, true, false, "idle"},
      {SessionState::kReady, 5, false, true, false, "ready"},
      {SessionState::kSynchronized, 5, true, true, false, "synchronized"},
      {SessionState::kVisible, 5, true, true, false, "visible"},
      {SessionState::kFocused, 20, true, true, false, "focused+steady"},
      {SessionState::kVisible, 10, false, true, false, "focus-lost"},
      {SessionState::kFocused, 10, true, true, true, "focus-restored+recenter"},
      {SessionState::kFocused, 10, false, false, false, "headset-absent"},
      {SessionState::kFocused, 10, true, true, false, "recovered"},
      {SessionState::kStopping, 5, false, true, false, "stopping"},
  };
}

std::vector<PhaseSpec> DefaultRealScript(std::uint64_t focused_frames) {
  return std::vector<PhaseSpec>{
      {SessionState::kIdle, 5, false, true, false, "idle"},
      {SessionState::kFocused, focused_frames, true, true, false, "focused"},
  };
}

struct SceneRenderer::Native {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> eye_tex[2];
  ComPtr<ID3D11Texture2D> eye_stage[2];
  ComPtr<ID3D11RenderTargetView> eye_rtv[2];
};

SceneRenderer::SceneRenderer() : native_(new Native()) {}

SceneRenderer::~SceneRenderer() {
  shutdown();
  delete native_;
  native_ = nullptr;
}

bool SceneRenderer::startup(std::uint32_t width, std::uint32_t height,
                            std::string* error) {
  if (ready_ || width == 0 || height == 0) {
    if (error != nullptr) *error = "bad dimensions or already started";
    return false;
  }
  static constexpr D3D_FEATURE_LEVEL kLevels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  static constexpr D3D_DRIVER_TYPE kDrivers[] = {D3D_DRIVER_TYPE_HARDWARE,
                                                D3D_DRIVER_TYPE_WARP};
  HRESULT hr = E_FAIL;
  for (std::size_t d = 0; d < 2; ++d) {
    hr = D3D11CreateDevice(
        nullptr, kDrivers[d], nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kLevels, 2, D3D11_SDK_VERSION, &native_->device, nullptr,
        &native_->context);
    if (SUCCEEDED(hr) && native_->device != nullptr &&
        native_->context != nullptr) {
      break;
    }
  }
  if (FAILED(hr) || native_->device == nullptr ||
      native_->context == nullptr) {
    if (error != nullptr) {
      std::ostringstream out;
      out << "D3D11CreateDevice failed (hr=0x" << std::hex
          << static_cast<unsigned>(hr) << ")";
      *error = out.str();
    }
    native_->device.Reset();
    native_->context.Reset();
    return false;
  }

  D3D11_TEXTURE2D_DESC rt{};
  rt.Width = static_cast<UINT>(width);
  rt.Height = static_cast<UINT>(height);
  rt.MipLevels = 1;
  rt.ArraySize = 1;
  rt.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rt.SampleDesc.Count = 1;
  rt.Usage = D3D11_USAGE_DEFAULT;
  rt.BindFlags = D3D11_BIND_RENDER_TARGET;
  D3D11_TEXTURE2D_DESC stage = rt;
  stage.Usage = D3D11_USAGE_STAGING;
  stage.BindFlags = 0;
  stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  for (int eye = 0; eye < 2; ++eye) {
    if (FAILED(native_->device->CreateTexture2D(
            &rt, nullptr, &native_->eye_tex[eye])) ||
        FAILED(native_->device->CreateRenderTargetView(
            native_->eye_tex[eye].Get(), nullptr,
            &native_->eye_rtv[eye])) ||
        FAILED(native_->device->CreateTexture2D(
            &stage, nullptr, &native_->eye_stage[eye]))) {
      if (error != nullptr) *error = "per-eye target creation failed";
      shutdown();
      return false;
    }
  }
  width_ = width;
  height_ = height;
  ready_ = true;
  return true;
}

void SceneRenderer::shutdown() {
  if (native_ != nullptr) {
    for (int eye = 0; eye < 2; ++eye) {
      native_->eye_rtv[eye].Reset();
      native_->eye_tex[eye].Reset();
      native_->eye_stage[eye].Reset();
    }
    native_->context.Reset();
    native_->device.Reset();
  }
  width_ = 0;
  height_ = 0;
  ready_ = false;
}

bool SceneRenderer::ready() const { return ready_; }
std::uint32_t SceneRenderer::width() const { return width_; }
std::uint32_t SceneRenderer::height() const { return height_; }

EyeColors SceneRenderer::ExpectedColors(std::uint32_t eye,
                                        float yaw_radians) {
  EyeColors out{};
  const float* clear = (eye == 0) ? kClearLeft : kClearRight;
  const float* quad = (eye == 0) ? kQuadLeft : kQuadRight;
  const float brightness = QuadBrightness(yaw_radians);
  for (int i = 0; i < 4; ++i) out.clear_rgba[i] = FloatToByte(clear[i]);
  for (int i = 0; i < 3; ++i)
    out.quad_rgba[i] = FloatToByte(quad[i] * brightness);
  out.quad_rgba[3] = 255;
  return out;
}

bool SceneRenderer::renderEye(std::uint32_t eye, float yaw_radians) {
  if (!ready_ || eye > 1) return false;
  const EyeColors want = ExpectedColors(eye, yaw_radians);
  const float clear[4] = {
      static_cast<float>(want.clear_rgba[0]) / 255.0f,
      static_cast<float>(want.clear_rgba[1]) / 255.0f,
      static_cast<float>(want.clear_rgba[2]) / 255.0f,
      1.0f,
  };
  ID3D11RenderTargetView* rtv = native_->eye_rtv[eye].Get();
  native_->context->OMSetRenderTargets(1, &rtv, nullptr);
  native_->context->ClearRenderTargetView(rtv, clear);

  // Centered quad covering the middle quarter of the target.
  const std::uint32_t qw = width_ / 4;
  const std::uint32_t qh = height_ / 4;
  const std::uint32_t qx = width_ / 2 - qw / 2;
  const std::uint32_t qy = height_ / 2 - qh / 2;
  std::vector<std::uint8_t> quad(static_cast<std::size_t>(qw) *
                                 static_cast<std::size_t>(qh) * 4);
  for (std::size_t p = 0; p < quad.size(); p += 4) {
    quad[p + 0] = want.quad_rgba[0];
    quad[p + 1] = want.quad_rgba[1];
    quad[p + 2] = want.quad_rgba[2];
    quad[p + 3] = want.quad_rgba[3];
  }
  D3D11_BOX box{};
  box.left = qx;
  box.top = qy;
  box.front = 0;
  box.right = qx + qw;
  box.bottom = qy + qh;
  box.back = 1;
  native_->context->UpdateSubresource(
      native_->eye_tex[eye].Get(), 0, &box, quad.data(),
      static_cast<UINT>(qw * 4), static_cast<UINT>(qw * qh * 4));
  native_->context->CopyResource(native_->eye_stage[eye].Get(),
                                 native_->eye_tex[eye].Get());
  // Flush the immediate context so a failure surfaces here, on the worker.
  native_->context->Flush();
  return true;
}

bool SceneRenderer::readback(std::uint32_t eye, std::uint8_t corner_rgba[4],
                             std::uint8_t center_rgba[4]) {
  if (!ready_ || eye > 1) return false;
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(native_->context->Map(native_->eye_stage[eye].Get(), 0,
                                   D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }
  const std::uint8_t* rows =
      static_cast<const std::uint8_t*>(mapped.pData);
  const std::size_t pitch = mapped.RowPitch;
  const std::size_t corner_offset = static_cast<std::size_t>(8) * pitch + 8 * 4;
  const std::size_t center_offset =
      static_cast<std::size_t>(height_ / 2) * pitch +
      static_cast<std::size_t>(width_ / 2) * 4;
  std::memcpy(corner_rgba, rows + corner_offset, 4);
  std::memcpy(center_rgba, rows + center_offset, 4);
  native_->context->Unmap(native_->eye_stage[eye].Get(), 0);
  return true;
}

SceneSession::SceneSession(IXrBackend* backend, SceneRenderer* renderer)
    : backend_(backend), renderer_(renderer) {}

SceneSession::~SceneSession() = default;

void SceneSession::setPhases(const std::vector<PhaseSpec>& phases) {
  phases_ = phases;
}

void SceneSession::requestRecenter() { recenter_requested_.store(true); }

const SessionStats& SceneSession::stats() const { return stats_; }

void SceneSession::run() {
  stats_ = SessionStats{};
  stats_.caller_id = std::this_thread::get_id();
  std::thread worker(&SceneSession::workerMain, this);
  worker.join();
  stats_.shutdown_order.push_back("worker_joined");
}

void SceneSession::workerMain() {
  stats_.worker_id = std::this_thread::get_id();
  stats_.worker_ran = true;
  std::uint64_t frame = 0;
  SessionState current = SessionState::kUnknown;
  bool first_timing = true;
  bool input_sampled = false;

  for (std::size_t p = 0; p < phases_.size(); ++p) {
    const PhaseSpec& phase = phases_[p];
    if (phase.state != current) {
      std::ostringstream t;
      t << SessionStateName(current) << "->" << SessionStateName(phase.state)
        << " @frame " << frame << " (" << phase.label << ")";
      stats_.transitions.push_back(t.str());
      current = phase.state;
    }
    if (phase.recenter_at_start && phase.backend_reachable) {
      backend_->recenter();
      stats_.recenter_applied = true;
      stats_.recenter_frame = frame;
    }
    for (std::uint64_t i = 0; i < phase.frames; ++i) {
      if (!phase.backend_reachable) {
        // Headset absent: no backend calls at all. The backend is
        // unreachable; the harness just counts gated frames.
        ++stats_.gated_absent;
        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      if (recenter_requested_.exchange(false)) {
        backend_->recenter();
        stats_.recenter_applied = true;
        stats_.recenter_frame = frame;
      }
      // xrWaitFrame and every other backend call stays on this worker
      // (Key Decision 3); the coordinating thread never calls them.
      const FrameTiming timing = backend_->waitFrame();
      if (first_timing) {
        stats_.first_time_ns = timing.predicted_display_time_ns;
        first_timing = false;
      }
      if (timing.predicted_display_time_ns <= stats_.last_time_ns &&
          frame > 0) {
        stats_.monotonic = false;
      }
      stats_.last_time_ns = timing.predicted_display_time_ns;
      ++frame;

      if (!timing.should_render) {
        ++stats_.waited_not_ready;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      if (!backend_->beginFrame()) {
        ++stats_.begin_failed;
        continue;
      }
      const LocatedViews local = backend_->locateViews(Space::kLocal);
      const LocatedViews stage = backend_->locateViews(Space::kStage);
      const LocatedViews view = backend_->locateViews(Space::kView);
      if (local.views[0].pose.orientation.w == 0.0f &&
          local.views[1].pose.orientation.w == 0.0f) {
        stats_.spaces_ok = false;
      }
      if (local.views[0].fov.angle_left != local.views[1].fov.angle_left ||
          local.views[0].fov.angle_right != local.views[1].fov.angle_right) {
        stats_.fov_asymmetric = true;
      }
      (void)stage;
      (void)view;

      if (!input_sampled && current == SessionState::kFocused) {
        stats_.sampled_left = backend_->controllerState(Hand::kLeft);
        stats_.sampled_right = backend_->controllerState(Hand::kRight);
        stats_.sampled_input = true;
        input_sampled = true;
      }

      if (phase.submit && renderer_ != nullptr && renderer_->ready()) {
        const float yaw =
            YawFromQuaternion(local.views[0].pose.orientation);
        bool rendered = true;
        for (std::uint32_t eye = 0; eye < backend_->viewCount(); ++eye) {
          const std::uint32_t image = backend_->acquireSwapchainImage(eye);
          (void)image;
          if (!renderer_->renderEye(eye, yaw)) rendered = false;
          // Verify pixels on every 5th submitted frame, both eyes.
          if ((stats_.rendered + 1) % 5 == 0) {
            std::uint8_t corner[4] = {};
            std::uint8_t center[4] = {};
            const EyeColors want =
                SceneRenderer::ExpectedColors(eye, yaw);
            ++stats_.pixels_verified;
            if (!renderer_->readback(eye, corner, center) ||
                !PixelsNear(corner, want.clear_rgba) ||
                !PixelsNear(center, want.quad_rgba)) {
              ++stats_.pixel_failures;
            }
          }
          backend_->releaseSwapchainImage(eye);
        }
        if (!rendered && stats_.error.empty())
          stats_.error = "renderEye failed";
        if (!backend_->endFrame(true)) {
          if (stats_.error.empty()) stats_.error = "endFrame(true) failed";
        } else {
          ++stats_.rendered;
        }
      } else {
        // Idle / unfocused / stopping: pump the loop without submitting.
        if (!backend_->endFrame(false)) {
          if (stats_.error.empty()) stats_.error = "endFrame(false) failed";
        } else {
          ++stats_.unsubmitted;
        }
      }
    }
  }
}

}  // namespace mecvr::openxr::scene
