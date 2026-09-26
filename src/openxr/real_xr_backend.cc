#include "openxr/real_xr_backend.h"

// Real OpenXR backend (plan T8, M1B). Standalone session against the
// registered runtime (VDXR on this machine): own D3D11 device, dedicated
// caller-owned XR worker thread, graceful headset-absent degradation.

// NOTE: d3d11.h must come first: openxr_platform.h uses LUID,
// D3D_FEATURE_LEVEL, and ID3D11Device in its D3D11 structs.
#pragma warning(push, 3)
#include <d3d11.h>
#include <dxgi.h>
#pragma warning(pop)
#define XR_USE_GRAPHICS_API_D3D11
#pragma warning(push, 3)
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#pragma warning(pop)

#include <cmath>
#include <vector>

namespace mecvr::openxr {
namespace {

// ---- Small mapping helpers (real API -> seam types) -----------------------

// NOTE: the seam types in xr_types.h share names with the real API types,
// so every real-API use here is ::-qualified to avoid picking up the seam.
XrVector3f ToSeamVec(const ::XrVector3f& v) {
  XrVector3f out;
  out.x = v.x;
  out.y = v.y;
  out.z = v.z;
  return out;
}

XrQuaternionf ToSeamQuat(const ::XrQuaternionf& q) {
  XrQuaternionf out;
  out.x = q.x;
  out.y = q.y;
  out.z = q.z;
  out.w = q.w;
  return out;
}

XrPosef ToSeamPose(const ::XrPosef& p) {
  XrPosef out;
  out.orientation = ToSeamQuat(p.orientation);
  out.position = ToSeamVec(p.position);
  return out;
}

XrFovf ToSeamFov(const ::XrFovf& f) {
  XrFovf out;
  out.angle_left = f.angleLeft;
  out.angle_right = f.angleRight;
  out.angle_up = f.angleUp;
  out.angle_down = f.angleDown;
  return out;
}

const char* ResultName(::XrResult r) {
  switch (r) {
    case XR_SUCCESS:
      return "XR_SUCCESS";
    case XR_TIMEOUT_EXPIRED:
      return "XR_TIMEOUT_EXPIRED";
    case XR_SESSION_LOSS_PENDING:
      return "XR_SESSION_LOSS_PENDING";
    case XR_EVENT_UNAVAILABLE:
      return "XR_EVENT_UNAVAILABLE";
    case XR_ERROR_VALIDATION_FAILURE:
      return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_RUNTIME_FAILURE:
      return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_OUT_OF_MEMORY:
      return "XR_ERROR_OUT_OF_MEMORY";
    case XR_ERROR_API_VERSION_UNSUPPORTED:
      return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_INITIALIZATION_FAILED:
      return "XR_ERROR_INITIALIZATION_FAILED";
    case XR_ERROR_FUNCTION_UNSUPPORTED:
      return "XR_ERROR_FUNCTION_UNSUPPORTED";
    case XR_ERROR_FEATURE_UNSUPPORTED:
      return "XR_ERROR_FEATURE_UNSUPPORTED";
    case XR_ERROR_EXTENSION_NOT_PRESENT:
      return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_LIMIT_REACHED:
      return "XR_ERROR_LIMIT_REACHED";
    case XR_ERROR_SIZE_INSUFFICIENT:
      return "XR_ERROR_SIZE_INSUFFICIENT";
    case XR_ERROR_HANDLE_INVALID:
      return "XR_ERROR_HANDLE_INVALID";
    case XR_ERROR_INSTANCE_LOST:
      return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_SESSION_RUNNING:
      return "XR_ERROR_SESSION_RUNNING";
    case XR_ERROR_SESSION_NOT_RUNNING:
      return "XR_ERROR_SESSION_NOT_RUNNING";
    case XR_ERROR_SESSION_LOST:
      return "XR_ERROR_SESSION_LOST";
    case XR_ERROR_SYSTEM_INVALID:
      return "XR_ERROR_SYSTEM_INVALID";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
      return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED:
      return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    default:
      return "XR_UNKNOWN";
  }
}

const char* SessionStateName(::XrSessionState s) {
  switch (s) {
    case XR_SESSION_STATE_UNKNOWN:
      return "UNKNOWN";
    case XR_SESSION_STATE_IDLE:
      return "IDLE";
    case XR_SESSION_STATE_READY:
      return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED:
      return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:
      return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:
      return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:
      return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING:
      return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:
      return "EXITING";
    default:
      return "UNRECOGNIZED";
  }
}

}  // namespace

struct RealOpenXRBackend::Native {
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE;
  XrSpace stage = XR_NULL_HANDLE;
  XrSpace view = XR_NULL_HANDLE;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  XrSwapchain chains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::vector<XrSwapchainImageD3D11KHR> images[2];
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  bool begun = false;
  bool stage_supported = false;
  PFN_xrGetD3D11GraphicsRequirementsKHR get_d3d11_requirements = nullptr;
  PFN_xrEnumerateDisplayRefreshRatesFB enum_refresh_rates = nullptr;
  PFN_xrGetDisplayRefreshRateFB get_refresh_rate = nullptr;
};

RealOpenXRBackend::RealOpenXRBackend() : native_(new Native()) {}

RealOpenXRBackend::~RealOpenXRBackend() {
  shutdown();
  delete native_;
  native_ = nullptr;
}

RealBackendDiagnostics RealOpenXRBackend::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return diagnostics_;
}

bool RealOpenXRBackend::stageAvailable() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return diagnostics_.stage_available;
}

bool RealOpenXRBackend::hasFocus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return native_->state == XR_SESSION_STATE_FOCUSED;
}

bool RealOpenXRBackend::startup() {
  std::lock_guard<std::mutex> lock(mutex_);
  RealBackendDiagnostics& d = diagnostics_;
  Native& n = *native_;

  // 1. Instance with the D3D11 extension (Key Decision 1).
  {
    XrApplicationInfo app{};
    const char* name = "MECVR";
    const char* engine = "MECVR";
    for (int i = 0; i < 128 && name[i] != '\0'; ++i) app.applicationName[i] = name[i];
    app.applicationVersion = 1;
    for (int i = 0; i < 128 && engine[i] != '\0'; ++i) app.engineName[i] = engine[i];
    app.engineVersion = 1;
    // Request 1.0 for maximum runtime compatibility (newer runtimes accept
    // it; some installed runtimes reject a 1.1 request outright).
    app.apiVersion = XR_API_VERSION_1_0;

    const char* extensions[] = {"XR_KHR_D3D11_enable"};
    XrInstanceCreateInfo create{};
    create.type = XR_TYPE_INSTANCE_CREATE_INFO;
    create.applicationInfo = app;
    create.enabledExtensionCount = 1;
    create.enabledExtensionNames = extensions;

    const XrResult r = xrCreateInstance(&create, &n.instance);
    if (r != XR_SUCCESS || n.instance == XR_NULL_HANDLE) {
      d.failure_reason = std::string("xrCreateInstance failed: ") + ResultName(r);
      return false;
    }
    XrInstanceProperties props{};
    props.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (xrGetInstanceProperties(n.instance, &props) == XR_SUCCESS) {
      d.runtime_name = props.runtimeName;
      const std::uint64_t v =
          static_cast<std::uint64_t>(props.runtimeVersion);
      d.runtime_version_major = static_cast<std::uint32_t>((v >> 48) & 0xffffu);
      d.runtime_version_minor = static_cast<std::uint32_t>((v >> 32) & 0xffffu);
      d.runtime_version_patch = static_cast<std::uint32_t>(v & 0xffffffffu);
    }
    d.instance_created = true;

    // Optional: refresh-rate extension function pointers (Key Decision 4).
    (void)xrGetInstanceProcAddr(
        n.instance, "xrEnumerateDisplayRefreshRatesFB",
        reinterpret_cast<PFN_xrVoidFunction*>(&n.enum_refresh_rates));
    (void)xrGetInstanceProcAddr(
        n.instance, "xrGetDisplayRefreshRateFB",
        reinterpret_cast<PFN_xrVoidFunction*>(&n.get_refresh_rate));
    (void)xrGetInstanceProcAddr(
        n.instance, "xrGetD3D11GraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&n.get_d3d11_requirements));
  }

  // 2. System (HMD). Absent headset -> graceful degradation, not an error.
  {
    XrSystemGetInfo info{};
    info.type = XR_TYPE_SYSTEM_GET_INFO;
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult r = xrGetSystem(n.instance, &info, &n.system);
    if (r != XR_SUCCESS) {
      d.failure_reason = std::string("xrGetSystem (HMD) failed: ") +
                         ResultName(r) +
                         " (headset absent or unreachable; degrading to diagnostics)";
      xrDestroyInstance(n.instance);
      n.instance = XR_NULL_HANDLE;
      return false;
    }
    d.system_acquired = true;
  }

  // 3. D3D11 graphics requirements: adapter LUID + min feature level gate.
  LUID required_luid{};
  D3D_FEATURE_LEVEL min_level = D3D_FEATURE_LEVEL_11_0;
  {
    if (n.get_d3d11_requirements == nullptr) {
      d.failure_reason = "xrGetD3D11GraphicsRequirementsKHR unavailable";
      xrDestroyInstance(n.instance);
      n.instance = XR_NULL_HANDLE;
      return false;
    }
    XrGraphicsRequirementsD3D11KHR req{};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    const XrResult r = n.get_d3d11_requirements(n.instance, n.system, &req);
    if (r != XR_SUCCESS) {
      d.failure_reason =
          std::string("xrGetD3D11GraphicsRequirementsKHR failed: ") + ResultName(r);
      xrDestroyInstance(n.instance);
      n.instance = XR_NULL_HANDLE;
      return false;
    }
    required_luid = req.adapterLuid;
    min_level = req.minFeatureLevel;
    d.requirements_queried = true;
  }

  // 4. Own D3D11 device on the runtime-required adapter (no game device).
  {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory)))) {
      d.failure_reason = "CreateDXGIFactory1 failed";
      xrDestroyInstance(n.instance);
      n.instance = XR_NULL_HANDLE;
      return false;
    }
    IDXGIAdapter1* match = nullptr;
    for (UINT i = 0;; ++i) {
      IDXGIAdapter1* adapter = nullptr;
      if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
          desc.AdapterLuid.LowPart == required_luid.LowPart &&
          desc.AdapterLuid.HighPart == required_luid.HighPart) {
        match = adapter;
        char narrow[128] = {};
        for (int c = 0; c < 127 && desc.Description[c] != L'\0'; ++c)
          narrow[c] = static_cast<char>(desc.Description[c]);
        d.adapter_description = narrow;
        d.adapter_vendor_id = desc.VendorId;
        d.adapter_device_id = desc.DeviceId;
        break;
      }
      adapter->Release();
    }
    factory->Release();
    if (match == nullptr) {
      d.failure_reason = "no DXGI adapter matches the runtime-required LUID";
      xrDestroyInstance(n.instance);
      n.instance = XR_NULL_HANDLE;
      return false;
    }
    // Ordered high->low; the runtime min is enforced via the obtained
    // level check below.
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_9_1;
    const HRESULT hr = D3D11CreateDevice(
        match, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
        &n.device, &obtained, &n.context);
    // Wide-string description already captured; release the adapter now.
    match->Release();
    if (FAILED(hr) || n.device == nullptr || obtained < min_level) {
      d.failure_reason = "D3D11CreateDevice on runtime adapter failed";
      n.device = nullptr;
      n.context = nullptr;
      xrDestroyInstance(n.instance);
      n.instance = XR_NULL_HANDLE;
      return false;
    }
    d.device_created = true;
  }

  // 5. Session bound to our own device.
  {
    XrGraphicsBindingD3D11KHR binding{};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
    binding.device = n.device;
    XrSessionCreateInfo info{};
    info.type = XR_TYPE_SESSION_CREATE_INFO;
    info.next = &binding;
    info.systemId = n.system;
    const XrResult r = xrCreateSession(n.instance, &info, &n.session);
    if (r != XR_SUCCESS) {
      d.failure_reason = std::string("xrCreateSession failed: ") + ResultName(r);
      if (n.context != nullptr) {
        n.context->Release();
        n.context = nullptr;
      }
      if (n.device != nullptr) {
        n.device->Release();
        n.device = nullptr;
      }
      xrDestroyInstance(n.instance);
      n.instance = XR_NULL_HANDLE;
      return false;
    }
    d.session_created = true;
  }

  // 6. Spaces: LOCAL + VIEW always; STAGE only where supported (Dec. 9).
  {
    std::uint32_t space_count = 0;
    (void)xrEnumerateReferenceSpaces(n.session, 0, &space_count, nullptr);
    std::vector<XrReferenceSpaceType> spaces(space_count,
                                             XR_REFERENCE_SPACE_TYPE_VIEW);
    if (space_count > 0) {
      (void)xrEnumerateReferenceSpaces(n.session, space_count, &space_count,
                                       spaces.data());
    }
    for (std::uint32_t i = 0; i < space_count; ++i) {
      if (spaces[i] == XR_REFERENCE_SPACE_TYPE_STAGE) n.stage_supported = true;
    }
    auto make_space = [&](XrReferenceSpaceType type, XrSpace* out) {
      XrReferenceSpaceCreateInfo info{};
      info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
      info.referenceSpaceType = type;
      info.poseInReferenceSpace.orientation.w = 1.0f;
      return xrCreateReferenceSpace(n.session, &info, out);
    };
    if (make_space(XR_REFERENCE_SPACE_TYPE_LOCAL, &n.local) != XR_SUCCESS ||
        make_space(XR_REFERENCE_SPACE_TYPE_VIEW, &n.view) != XR_SUCCESS ||
        (n.stage_supported &&
         make_space(XR_REFERENCE_SPACE_TYPE_STAGE, &n.stage) != XR_SUCCESS)) {
      d.failure_reason = "reference space creation failed";
      shutdown();
      return false;
    }
    d.stage_available = n.stage_supported;
  }

  // 7. Stereo view configs + per-eye swapchains.
  {
    std::uint32_t count = 0;
    if (xrEnumerateViewConfigurationViews(n.instance, n.system,
                                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          0, &count, nullptr) != XR_SUCCESS ||
        count != 2) {
      d.failure_reason = "stereo view configuration unavailable";
      shutdown();
      return false;
    }
    XrViewConfigurationView views[2] = {};
    for (int i = 0; i < 2; ++i) views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (xrEnumerateViewConfigurationViews(
            n.instance, n.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            2, &count, views) != XR_SUCCESS ||
        count != 2) {
      d.failure_reason = "stereo view configuration query failed";
      shutdown();
      return false;
    }
    view_count_ = 2;
    for (int i = 0; i < 2; ++i) {
      view_configs_[i].recommended_width = views[i].recommendedImageRectWidth;
      view_configs_[i].recommended_height = views[i].recommendedImageRectHeight;
      view_configs_[i].max_width = views[i].maxImageRectWidth;
      view_configs_[i].max_height = views[i].maxImageRectHeight;
    }

    std::uint32_t format_count = 0;
    (void)xrEnumerateSwapchainFormats(n.session, 0, &format_count, nullptr);
    std::vector<std::int64_t> formats(
        format_count > 0 ? format_count : 1, 0);
    if (format_count > 0) {
      (void)xrEnumerateSwapchainFormats(n.session, format_count,
                                        &format_count, formats.data());
    }
    std::int64_t chosen = formats[0];
    for (std::uint32_t i = 0; i < format_count; ++i) {
      if (formats[i] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        chosen = formats[i];
        break;
      }
    }

    for (int i = 0; i < 2; ++i) {
      XrSwapchainCreateInfo info{};
      info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
      info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
      info.format = chosen;
      info.sampleCount = views[i].recommendedSwapchainSampleCount;
      info.width = views[i].recommendedImageRectWidth;
      info.height = views[i].recommendedImageRectHeight;
      info.faceCount = 1;
      info.arraySize = 1;
      info.mipCount = 1;
      if (xrCreateSwapchain(n.session, &info, &n.chains[i]) != XR_SUCCESS) {
        d.failure_reason = "swapchain creation failed";
        shutdown();
        return false;
      }
      std::uint32_t image_count = 0;
      (void)xrEnumerateSwapchainImages(n.chains[i], 0, &image_count, nullptr);
      n.images[i].resize(image_count);
      for (std::uint32_t k = 0; k < image_count; ++k)
        n.images[i][k].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
      if (image_count == 0 ||
          xrEnumerateSwapchainImages(
              n.chains[i], image_count, &image_count,
              reinterpret_cast<XrSwapchainImageBaseHeader*>(n.images[i].data())) !=
              XR_SUCCESS) {
        d.failure_reason = "swapchain image enumeration failed";
        shutdown();
        return false;
      }
    }
  }

  d.failure_reason.clear();
  d.session_state = SessionStateName(n.state);
  running_.store(true);
  return true;
}

void RealOpenXRBackend::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  running_.store(false);
  for (int i = 0; i < 2; ++i) {
    if (n.chains[i] != XR_NULL_HANDLE) {
      xrDestroySwapchain(n.chains[i]);
      n.chains[i] = XR_NULL_HANDLE;
    }
    n.images[i].clear();
  }
  if (n.local != XR_NULL_HANDLE) {
    xrDestroySpace(n.local);
    n.local = XR_NULL_HANDLE;
  }
  if (n.stage != XR_NULL_HANDLE) {
    xrDestroySpace(n.stage);
    n.stage = XR_NULL_HANDLE;
  }
  if (n.view != XR_NULL_HANDLE) {
    xrDestroySpace(n.view);
    n.view = XR_NULL_HANDLE;
  }
  if (n.session != XR_NULL_HANDLE) {
    xrDestroySession(n.session);
    n.session = XR_NULL_HANDLE;
  }
  if (n.context != nullptr) {
    n.context->Release();
    n.context = nullptr;
  }
  if (n.device != nullptr) {
    n.device->Release();
    n.device = nullptr;
  }
  if (n.instance != XR_NULL_HANDLE) {
    xrDestroyInstance(n.instance);
    n.instance = XR_NULL_HANDLE;
  }
  n.begun = false;
  n.state = XR_SESSION_STATE_UNKNOWN;
  frame_open_ = false;
  view_count_ = 0;
}

bool RealOpenXRBackend::running() const { return running_.load(); }

// Polls and dispatches runtime events. Returns false on instance loss.
// Caller holds the backend mutex.
bool RealOpenXRBackend::PollEventsLocked(RealOpenXRBackend::Native& n,
                                         RealBackendDiagnostics& d) {
  XrEventDataBuffer event{};
  event.type = XR_TYPE_EVENT_DATA_BUFFER;
  while (xrPollEvent(n.instance, &event) == XR_SUCCESS) {
    switch (event.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        const auto& changed =
            reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
        n.state = changed.state;
        d.session_state = SessionStateName(n.state);
        if (n.state == XR_SESSION_STATE_FOCUSED) d.focus_received = true;
        if (n.state == XR_SESSION_STATE_READY && !n.begun) {
          XrSessionBeginInfo begin{};
          begin.type = XR_TYPE_SESSION_BEGIN_INFO;
          begin.primaryViewConfigurationType =
              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          if (xrBeginSession(n.session, &begin) == XR_SUCCESS) {
            n.begun = true;
            d.session_begun = true;
          } else {
            d.failure_reason = "xrBeginSession failed after READY";
          }
        } else if (n.state == XR_SESSION_STATE_STOPPING && n.begun) {
          (void)xrEndSession(n.session);
          n.begun = false;
        } else if (n.state == XR_SESSION_STATE_EXITING ||
                   n.state == XR_SESSION_STATE_LOSS_PENDING) {
          d.session_loss = true;
        }
        break;
      }
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        d.instance_loss = true;
        return false;
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
        // Runtime re-based a space; recreate ours to match.
        auto remake = [&](XrReferenceSpaceType type, XrSpace* space) {
          if (*space == XR_NULL_HANDLE) return;
          xrDestroySpace(*space);
          *space = XR_NULL_HANDLE;
          XrReferenceSpaceCreateInfo info{};
          info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
          info.referenceSpaceType = type;
          info.poseInReferenceSpace.orientation.w = 1.0f;
          (void)xrCreateReferenceSpace(n.session, &info, space);
        };
        remake(XR_REFERENCE_SPACE_TYPE_LOCAL, &n.local);
        remake(XR_REFERENCE_SPACE_TYPE_VIEW, &n.view);
        if (n.stage_supported) remake(XR_REFERENCE_SPACE_TYPE_STAGE, &n.stage);
        break;
      }
      default:
        break;
    }
    event.type = XR_TYPE_EVENT_DATA_BUFFER;
  }
  return true;
}

FrameTiming RealOpenXRBackend::waitFrame() {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || n.session == XR_NULL_HANDLE) return last_timing_;
  if (!PollEventsLocked(n, diagnostics_)) return last_timing_;
  // Deferred LOCAL re-baseline requested via recenter().
  if (recenter_requested_ && n.local != XR_NULL_HANDLE) {
    xrDestroySpace(n.local);
    n.local = XR_NULL_HANDLE;
    XrReferenceSpaceCreateInfo info{};
    info.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace.orientation.w = 1.0f;
    if (xrCreateReferenceSpace(n.session, &info, &n.local) == XR_SUCCESS)
      recenter_requested_ = false;
  }
  if (!n.begun || diagnostics_.session_loss || diagnostics_.instance_loss) {
    last_timing_.should_render = false;
    return last_timing_;
  }
  XrFrameState state{};
  state.type = XR_TYPE_FRAME_STATE;
  if (xrWaitFrame(n.session, nullptr, &state) != XR_SUCCESS) {
    last_timing_.should_render = false;
    return last_timing_;
  }
  FrameTiming timing;
  timing.predicted_display_time_ns = state.predictedDisplayTime;
  timing.predicted_display_period_ns = state.predictedDisplayPeriod;
  timing.frame_index = ++diagnostics_.frames_pumped;
  timing.should_render =
      state.shouldRender != XR_FALSE && !diagnostics_.session_loss;
  if (state.predictedDisplayPeriod > 0) {
    const double period = static_cast<double>(state.predictedDisplayPeriod);
    ema_period_ns_ =
        ema_period_ns_ == 0.0 ? period : 0.9 * ema_period_ns_ + 0.1 * period;
  }
  last_timing_ = timing;
  return timing;
}

bool RealOpenXRBackend::beginFrame() {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || !n.begun || frame_open_) return false;
  XrFrameBeginInfo info{};
  info.type = XR_TYPE_FRAME_BEGIN_INFO;
  if (xrBeginFrame(n.session, &info) != XR_SUCCESS) return false;
  frame_open_ = true;
  return true;
}

LocatedViews RealOpenXRBackend::locateViews(Space space) {
  std::lock_guard<std::mutex> lock(mutex_);
  LocatedViews out;
  out.space = space;
  Native& n = *native_;
  if (!running_.load() || !n.begun) return out;
  XrSpace base = n.local;
  if (space == Space::kView) {
    base = n.view;
  } else if (space == Space::kStage) {
    // STAGE where available, LOCAL fallback (Key Decision 9).
    base = (n.stage_supported && n.stage != XR_NULL_HANDLE) ? n.stage : n.local;
  }
  XrViewLocateInfo locate{};
  locate.type = XR_TYPE_VIEW_LOCATE_INFO;
  locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate.displayTime = last_timing_.predicted_display_time_ns;
  locate.space = base;
  XrViewState state{};
  state.type = XR_TYPE_VIEW_STATE;
  ::XrView raw[2] = {};
  raw[0].type = XR_TYPE_VIEW;
  raw[1].type = XR_TYPE_VIEW;
  std::uint32_t count = 0;
  if (xrLocateViews(n.session, &locate, &state, 2, &count, raw) !=
          XR_SUCCESS ||
      count != 2) {
    return out;
  }
  out.sample_time_ns = last_timing_.predicted_display_time_ns;
  for (int i = 0; i < 2; ++i) {
    out.views[i].pose = ToSeamPose(raw[i].pose);
    out.views[i].fov = ToSeamFov(raw[i].fov);
  }
  return out;
}

std::uint32_t RealOpenXRBackend::acquireSwapchainImage(
    std::uint32_t view_index) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || view_index >= view_count_) return 0u;
  std::uint32_t index = 0;
  if (xrAcquireSwapchainImage(n.chains[view_index], nullptr, &index) !=
      XR_SUCCESS) {
    return 0u;
  }
  XrSwapchainImageWaitInfo wait{};
  wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
  wait.timeout = XR_INFINITE_DURATION;
  if (xrWaitSwapchainImage(n.chains[view_index], &wait) != XR_SUCCESS) return 0u;
  return index;
}

void RealOpenXRBackend::releaseSwapchainImage(std::uint32_t view_index) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || view_index >= view_count_) return;
  XrSwapchainImageReleaseInfo info{};
  info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
  (void)xrReleaseSwapchainImage(n.chains[view_index], &info);
}

bool RealOpenXRBackend::endFrame(bool submitted) {
  (void)submitted;  // M1B submits zero layers; scene layers arrive in M2B.
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || !frame_open_) return false;
  XrFrameEndInfo info{};
  info.type = XR_TYPE_FRAME_END_INFO;
  info.displayTime = last_timing_.predicted_display_time_ns;
  info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  info.layerCount = 0;
  info.layers = nullptr;
  const bool ok = xrEndFrame(n.session, &info) == XR_SUCCESS;
  frame_open_ = false;
  return ok;
}

void RealOpenXRBackend::recenter() {
  // LOCAL re-baseline request path: the worker applies it inside waitFrame
  // (recreate LOCAL space at the runtime's current origin).
  std::lock_guard<std::mutex> lock(mutex_);
  recenter_requested_ = true;
}

ControllerState RealOpenXRBackend::controllerState(Hand hand) {
  (void)hand;
  // M1B boundary: no action set is created yet, so there is no hardware
  // state to report. Grip/boolean actions via xrCreateActionSpace arrive
  // with the T4 input wiring; the seam stays neutral until then.
  ControllerState state;
  state.grip_pose = IdentityPose();
  state.pose_valid = false;
  state.buttons = 0u;
  return state;
}

float RealOpenXRBackend::displayFrequencyHz() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const Native& n = *native_;
  // Prefer the runtime-exposed rate (Key Decision 4); fall back to the
  // xrWaitFrame-derived cadence.
  if (n.session != XR_NULL_HANDLE && n.enum_refresh_rates != nullptr &&
      n.get_refresh_rate != nullptr) {
    std::uint32_t count = 0;
    if (n.enum_refresh_rates(n.session, 0, &count, nullptr) == XR_SUCCESS &&
        count > 0) {
      float rate = 0.0f;
      if (n.get_refresh_rate(n.session, &rate) == XR_SUCCESS && rate > 0.0f)
        return rate;
    }
  }
  if (ema_period_ns_ > 0.0)
    return static_cast<float>(1000000000.0 / ema_period_ns_);
  return 0.0f;
}

ViewConfig RealOpenXRBackend::viewConfig(std::uint32_t view_index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ViewConfig config;
  if (view_index < view_count_ && view_index < 2) config = view_configs_[view_index];
  return config;
}

std::uint32_t RealOpenXRBackend::viewCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return view_count_;
}

}  // namespace mecvr::openxr
