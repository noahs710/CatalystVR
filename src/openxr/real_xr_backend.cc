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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  // Last located raw views + base space, cached for the projection layer
  // built in endFrame(true). Views are re-located every tick; the cache
  // always reflects the most recent successful locateViews.
  ::XrView last_raw_views[2];
  bool last_raw_valid = false;
  XrSpace last_base_space = XR_NULL_HANDLE;
  // Live-upload staging (M2B): per-view CPU-write texture at swapchain
  // dims/format + the currently acquired image index. -1 = none acquired.
  ID3D11Texture2D* staging[2] = {nullptr, nullptr};
  std::uint32_t staging_w[2] = {0, 0};
  std::uint32_t staging_h[2] = {0, 0};
  std::int64_t acquired_index[2] = {-1, -1};
  DXGI_FORMAT swap_format = DXGI_FORMAT_UNKNOWN;
  // Quad-layer mono presentation (M2B): one chain + staging; the worker's
  // two aliased view indices (0,1) map onto this single chain — acquire(0)
  // acquires, acquire(1) aliases; upload(0) uploads, upload(1) is a no-op;
  // release(0) releases, release(1) is a no-op.
  bool quad_enabled = false;
  XrSwapchain quad_chain = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageD3D11KHR> quad_images;
  ID3D11Texture2D* quad_staging = nullptr;
  std::uint32_t quad_w = 0;
  std::uint32_t quad_h = 0;
  std::int64_t quad_acquired = -1;
  bool quad_tried = false;  // Lazy-enable attempt done (env read once).
  bool quad_local = false;  // World-locked LOCAL pose vs head-locked VIEW.
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
    // Prefer UNORM 1:1 with captured game bytes (no color shift); SRGB
    // second; runtime default last. M2 shows game pixels, not a grade.
    const std::int64_t kPreference[] = {DXGI_FORMAT_R8G8B8A8_UNORM,
                                        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    std::int64_t chosen = formats[0];
    for (std::uint32_t p = 0; p < 2; ++p) {
      bool found = false;
      for (std::uint32_t i = 0; i < format_count; ++i) {
        if (formats[i] == kPreference[p]) {
          chosen = formats[i];
          found = true;
          break;
        }
      }
      if (found) break;
    }
    n.swap_format = static_cast<DXGI_FORMAT>(chosen);

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
    if (n.staging[i] != nullptr) {
      n.staging[i]->Release();
      n.staging[i] = nullptr;
    }
    n.staging_w[i] = 0;
    n.staging_h[i] = 0;
    n.acquired_index[i] = -1;
  }
  DestroyQuadLocked(n);
  n.last_raw_valid = false;
  n.last_base_space = XR_NULL_HANDLE;
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
    n.last_raw_views[i] = raw[i];
  }
  // [CONV] one-shot per-eye frustum dump: rules out garbage poses/FOVs as
  // the dizziness source (visible in DebugView / debugger output window).
  static bool conv_dumped = false;
  if (!conv_dumped) {
    conv_dumped = true;
    char conv[512];
    std::snprintf(
        conv, sizeof(conv),
        "[CONV] eye0 pos=(%.4f,%.4f,%.4f) fov=(%.3f,%.3f,%.3f,%.3f) "
        "eye1 pos=(%.4f,%.4f,%.4f) fov=(%.3f,%.3f,%.3f,%.3f)\n",
        raw[0].pose.position.x, raw[0].pose.position.y,
        raw[0].pose.position.z, raw[0].fov.angleLeft, raw[0].fov.angleRight,
        raw[0].fov.angleUp, raw[0].fov.angleDown, raw[1].pose.position.x,
        raw[1].pose.position.y, raw[1].pose.position.z,
        raw[1].fov.angleLeft, raw[1].fov.angleRight, raw[1].fov.angleUp,
        raw[1].fov.angleDown);
    OutputDebugStringA(conv);
  }
  n.last_raw_valid = true;
  n.last_base_space = base;
  return out;
}

// Quad mono presentation (M2B convergence fix). Identical pixels through
// two IPD-offset projection frustums disagree per eye (dizzying); a single
// compositor quad lets the runtime render each eye's view of ONE image
// natively — correct convergence with mono content, no stereo work.
// Default ON; MECVR_MONO_LAYER=projection restores projection layers
// (A/B + M6 experiments). Helpers assume mutex_ is held.
bool RealOpenXRBackend::QuadWanted() {
  char v[32] = {};
  const DWORD n = GetEnvironmentVariableA("MECVR_MONO_LAYER", v, sizeof(v));
  return n == 0 || std::strcmp(v, "projection") != 0;
}

bool RealOpenXRBackend::QuadLocal() {
  char v[32] = {};
  const DWORD n = GetEnvironmentVariableA("MECVR_QUAD_SPACE", v, sizeof(v));
  return n > 0 && std::strcmp(v, "local") == 0;
}

void RealOpenXRBackend::DestroyQuadLocked(Native& n) {
  if (n.quad_chain != XR_NULL_HANDLE) {
    xrDestroySwapchain(n.quad_chain);
    n.quad_chain = XR_NULL_HANDLE;
  }
  n.quad_images.clear();
  if (n.quad_staging != nullptr) {
    n.quad_staging->Release();
    n.quad_staging = nullptr;
  }
  n.quad_w = 0;
  n.quad_h = 0;
  n.quad_acquired = -1;
  n.quad_enabled = false;
}

bool RealOpenXRBackend::CreateQuadLocked(Native& n, std::uint32_t width,
                                            std::uint32_t height,
                                            const char** fail_reason_out) {
  auto fail = [&](const char* reason) {
    if (fail_reason_out != nullptr) *fail_reason_out = reason;
    char dbg[160];
    std::snprintf(dbg, sizeof(dbg),
                  "[QUAD] create %ux%u failed: %s\n", width, height, reason);
    OutputDebugStringA(dbg);
    return false;
  };
  if (n.session == XR_NULL_HANDLE || width == 0 || height == 0)
    return fail("no-session-or-dims");
  std::uint32_t count = 0;
  if (xrEnumerateSwapchainFormats(n.session, 0, &count, nullptr) !=
          XR_SUCCESS ||
      count == 0)
    return fail("format-query");
  std::vector<std::int64_t> formats(count);
  if (xrEnumerateSwapchainFormats(n.session, count, &count, formats.data()) !=
      XR_SUCCESS)
    return fail("format-query");
  std::int64_t fmt = 0;
  for (const std::int64_t want :
       {static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM),
        static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB),
        static_cast<std::int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM),
        static_cast<std::int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)}) {
    bool found = false;
    for (const std::int64_t f : formats) {
      if (f == want) {
        found = true;
        break;
      }
    }
    if (found) {
      fmt = want;
      break;
    }
  }
  if (fmt == 0) fmt = formats[0];
  XrSwapchainCreateInfo ci{};
  ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
  // COLOR_ATTACHMENT_BIT only — exactly the proven eye-chain flags.
  // SAMPLED/TRANSFER_DST got the creation rejected by VDXR (silent
  // projection fallback, still-dizzy image); D3D11 CopySubresourceRegion
  // needs no transfer flag.
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  ci.format = fmt;
  ci.sampleCount = 1;
  ci.width = width;
  ci.height = height;
  ci.faceCount = 1;
  ci.arraySize = 1;
  ci.mipCount = 1;
  const XrResult create_result =
      xrCreateSwapchain(n.session, &ci, &n.quad_chain);
  if (create_result != XR_SUCCESS) {
    n.quad_chain = XR_NULL_HANDLE;
    // Static buffer: reason must outlive this call for diagnostics.
    static char create_fail[64];
    std::snprintf(create_fail, sizeof(create_fail), "xrCreateSwapchain=%s",
                  ResultName(create_result));
    return fail(create_fail);
  }
  std::uint32_t icount = 0;
  if (xrEnumerateSwapchainImages(n.quad_chain, 0, &icount, nullptr) !=
      XR_SUCCESS) {
    DestroyQuadLocked(n);
    return fail("image-query");
  }
  n.quad_images.clear();
  for (std::uint32_t i = 0; i < icount; ++i) {
    XrSwapchainImageD3D11KHR img{};
    img.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    n.quad_images.push_back(img);
  }
  if (xrEnumerateSwapchainImages(
          n.quad_chain, icount, &icount,
          reinterpret_cast<XrSwapchainImageBaseHeader*>(
              n.quad_images.data())) != XR_SUCCESS) {
    DestroyQuadLocked(n);
    return fail("image-enumerate");
  }
  n.swap_format = static_cast<DXGI_FORMAT>(fmt);  // Only on success.
  n.quad_w = width;
  n.quad_h = height;
  n.quad_enabled = true;
  char ok[96];
  std::snprintf(ok, sizeof(ok), "[QUAD] chain %ux%u ready, %u images\n",
                width, height, icount);
  OutputDebugStringA(ok);
  return true;
}

bool RealOpenXRBackend::enableQuadLayer(std::uint32_t width,
                                        std::uint32_t height) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load()) return false;
  if (n.quad_enabled && n.quad_w == width && n.quad_h == height) return true;
  DestroyQuadLocked(n);
  n.quad_tried = true;
  return CreateQuadLocked(n, width, height, nullptr);
}

bool RealOpenXRBackend::ensureMonoLayer(std::uint32_t width,
                                        std::uint32_t height) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || width == 0 || height == 0) return true;
  // Clamp to the runtime's max swapchain extent, aspect preserved.
  std::uint32_t qw = width;
  std::uint32_t qh = height;
  const std::uint32_t mw =
      view_count_ > 0 ? view_configs_[0].max_width : 0;
  const std::uint32_t mh =
      view_count_ > 0 ? view_configs_[0].max_height : 0;
  if (mw > 0 && mh > 0 && (qw > mw || qh > mh)) {
    // Plain comparisons: windows.h min/max macros break std::min/max.
    double s = static_cast<double>(mw) / static_cast<double>(qw);
    const double sh = static_cast<double>(mh) / static_cast<double>(qh);
    if (sh < s) s = sh;
    qw = static_cast<std::uint32_t>(static_cast<double>(qw) * s);
    qh = static_cast<std::uint32_t>(static_cast<double>(qh) * s);
    if (qw < 1) qw = 1;
    if (qh < 1) qh = 1;
  }
  // Records the active composition path for the status line.
  auto note_layer = [&](bool created, const char* reason) {
    if (created) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "quad %ux%u", n.quad_w, n.quad_h);
      diagnostics_.mono_layer = buf;
      diagnostics_.mono_space = n.quad_local ? "local" : "view";
    } else {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "projection (quad: %s)",
                    reason != nullptr ? reason : "unknown");
      diagnostics_.mono_layer = buf;
      diagnostics_.mono_space = "n-a";
    }
  };
  if (!n.quad_tried) {
    n.quad_tried = true;
    n.quad_local = QuadLocal();  // MECVR_QUAD_SPACE=local or head-locked.
    if (!QuadWanted()) {
      diagnostics_.mono_layer = "projection (env)";
      diagnostics_.mono_space = "n-a";
    } else {
      const char* reason = nullptr;
      note_layer(CreateQuadLocked(n, qw, qh, &reason), reason);
    }
  } else if (n.quad_enabled && (n.quad_w != qw || n.quad_h != qh)) {
    // Game resized mid-session: rebuild before this tick's acquires.
    DestroyQuadLocked(n);
    const char* reason = nullptr;
    note_layer(CreateQuadLocked(n, qw, qh, &reason), reason);
  }
  return true;  // Quad failure falls back to the projection path.
}

std::uint32_t RealOpenXRBackend::acquireSwapchainImage(
    std::uint32_t view_index) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || view_index >= view_count_) return 0u;
  if (n.quad_enabled) {
    // Single chain: view 0 acquires, view 1 aliases the same image.
    // acquired_index[] mirrors the quad image so the upload guard passes.
    if (view_index == 1) {
      if (n.quad_acquired < 0) return 0u;
      n.acquired_index[1] = n.quad_acquired;
      return static_cast<std::uint32_t>(n.quad_acquired);
    }
    std::uint32_t qindex = 0;
    if (xrAcquireSwapchainImage(n.quad_chain, nullptr, &qindex) != XR_SUCCESS)
      return 0u;
    XrSwapchainImageWaitInfo wait{};
    wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wait.timeout = XR_INFINITE_DURATION;
    if (xrWaitSwapchainImage(n.quad_chain, &wait) != XR_SUCCESS) return 0u;
    n.quad_acquired = static_cast<std::int64_t>(qindex);
    n.acquired_index[0] = n.quad_acquired;
    return qindex;
  }
  std::uint32_t index = 0;
  if (xrAcquireSwapchainImage(n.chains[view_index], nullptr, &index) !=
      XR_SUCCESS) {
    return 0u;
  }
  XrSwapchainImageWaitInfo wait{};
  wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
  wait.timeout = XR_INFINITE_DURATION;
  if (xrWaitSwapchainImage(n.chains[view_index], &wait) != XR_SUCCESS) return 0u;
  n.acquired_index[view_index] = static_cast<std::int64_t>(index);
  return index;
}

bool RealOpenXRBackend::uploadEyeImage(std::uint32_t view_index,
                                       const std::uint8_t* rgba,
                                       std::uint32_t width,
                                       std::uint32_t height) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || view_index >= view_count_ || rgba == nullptr ||
      width == 0 || height == 0 || n.device == nullptr ||
      n.context == nullptr || n.acquired_index[view_index] < 0 ||
      static_cast<std::size_t>(n.acquired_index[view_index]) >=
          n.images[view_index].size()) {
    return false;
  }
  if (n.quad_enabled) {
    // Single chain: view 1 aliases view 0 — one upload, then no-op.
    // Sizing/resize handled in ensureMonoLayer (called pre-acquire).
    if (view_index == 1) return true;
    const bool exact =
        (width == n.quad_w && height == n.quad_h);
    if (n.quad_staging == nullptr) {
      D3D11_TEXTURE2D_DESC sd{};
      sd.Width = n.quad_w;
      sd.Height = n.quad_h;
      sd.MipLevels = 1;
      sd.ArraySize = 1;
      sd.Format = n.swap_format;
      sd.SampleDesc.Count = 1;
      sd.Usage = D3D11_USAGE_DYNAMIC;
      sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      if (FAILED(n.device->CreateTexture2D(&sd, nullptr, &n.quad_staging)) ||
          n.quad_staging == nullptr) {
        return false;
      }
    }
    // 1:1 fast path: chain is sized to the source, so rows copy straight
    // across (no resample, no letterbox) — ~2 ms vs ~77 ms. When the chain
    // was clamped below source dims, nearest-neighbor downscale instead
    // (aspect preserved by the clamp; M2 transport proof, not a grade).
    D3D11_MAPPED_SUBRESOURCE qmapped{};
    if (FAILED(n.context->Map(n.quad_staging, 0, D3D11_MAP_WRITE_DISCARD, 0,
                              &qmapped))) {
      return false;
    }
    const std::uint8_t* qsrc = rgba;
    std::uint8_t* qdst = static_cast<std::uint8_t*>(qmapped.pData);
    if (exact) {
      const std::size_t qrow = static_cast<std::size_t>(width) * 4;
      for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(qdst + static_cast<std::size_t>(y) * qmapped.RowPitch,
                    qsrc + static_cast<std::size_t>(y) * qrow, qrow);
      }
    } else {
      for (std::uint32_t y = 0; y < n.quad_h; ++y) {
        const std::uint32_t sy =
            static_cast<std::uint32_t>((static_cast<double>(y) * height) /
                                       n.quad_h);
        const std::uint8_t* srow =
            qsrc + static_cast<std::size_t>(sy < height ? sy : height - 1) *
                       width * 4;
        std::uint8_t* drow =
            qdst + static_cast<std::size_t>(y) * qmapped.RowPitch;
        for (std::uint32_t x = 0; x < n.quad_w; ++x) {
          const std::uint32_t sx =
              static_cast<std::uint32_t>((static_cast<double>(x) * width) /
                                         n.quad_w);
          const std::uint32_t clamped_sx = sx < width ? sx : width - 1;
          drow[x * 4 + 0] = srow[clamped_sx * 4 + 0];
          drow[x * 4 + 1] = srow[clamped_sx * 4 + 1];
          drow[x * 4 + 2] = srow[clamped_sx * 4 + 2];
          drow[x * 4 + 3] = srow[clamped_sx * 4 + 3];
        }
      }
    }
    n.context->Unmap(n.quad_staging, 0);
    if (n.quad_acquired < 0 ||
        static_cast<std::size_t>(n.quad_acquired) >= n.quad_images.size())
      return false;
    ID3D11Texture2D* qtex = n.quad_images[static_cast<std::size_t>(
        n.quad_acquired)].texture;
    if (qtex == nullptr) return false;
    n.context->CopySubresourceRegion(qtex, 0, 0, 0, 0, n.quad_staging, 0,
                                     nullptr);
    return true;
  }
  const std::uint32_t dst_w = view_configs_[view_index].recommended_width;
  const std::uint32_t dst_h = view_configs_[view_index].recommended_height;
  if (dst_w == 0 || dst_h == 0) return false;
  // (Re)create the CPU-write staging texture at swapchain dims/format.
  if (n.staging[view_index] == nullptr || n.staging_w[view_index] != dst_w ||
      n.staging_h[view_index] != dst_h) {
    if (n.staging[view_index] != nullptr) {
      n.staging[view_index]->Release();
      n.staging[view_index] = nullptr;
    }
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = dst_w;
    sd.Height = dst_h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = n.swap_format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_DYNAMIC;
    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(n.device->CreateTexture2D(&sd, nullptr,
                                         &n.staging[view_index])) ||
        n.staging[view_index] == nullptr) {
      return false;
    }
    n.staging_w[view_index] = dst_w;
    n.staging_h[view_index] = dst_h;
  }
  // Fit source width into the eye, preserve aspect, letterbox top/bottom
  // with black. M2 shows pixels 1:1 where they fit; no distortion ever.
  double scale = static_cast<double>(dst_w) / static_cast<double>(width);
  std::uint32_t draw_w = dst_w;
  std::uint32_t draw_h =
      static_cast<std::uint32_t>(static_cast<double>(height) * scale);
  if (draw_h > dst_h) {
    scale = static_cast<double>(dst_h) / static_cast<double>(height);
    draw_h = dst_h;
    draw_w = static_cast<std::uint32_t>(static_cast<double>(width) * scale);
  }
  const std::uint32_t off_x = (dst_w - draw_w) / 2;
  const std::uint32_t off_y = (dst_h - draw_h) / 2;
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(n.context->Map(n.staging[view_index], 0, D3D11_MAP_WRITE_DISCARD,
                            0, &mapped))) {
    return false;
  }
  // Bilinear sample; black outside the fitted rect (letterbox bars).
  auto* dst = static_cast<std::uint8_t*>(mapped.pData);
  memset(dst, 0, static_cast<std::size_t>(mapped.RowPitch) * dst_h);
  for (std::uint32_t y = 0; y < draw_h; ++y) {
    const double src_y = (static_cast<double>(y) + 0.5) / scale - 0.5;
    std::uint32_t y0 = static_cast<std::uint32_t>(src_y);
    std::uint32_t y1 = y0 + 1;
    const double fy = src_y - static_cast<double>(y0);
    if (y0 >= height) y0 = height - 1;
    if (y1 >= height) y1 = height - 1;
    std::uint8_t* row = dst + static_cast<std::size_t>(off_y + y) *
                                    mapped.RowPitch +
                        static_cast<std::size_t>(off_x) * 4;
    for (std::uint32_t x = 0; x < draw_w; ++x) {
      const double src_x = (static_cast<double>(x) + 0.5) / scale - 0.5;
      std::uint32_t x0 = static_cast<std::uint32_t>(src_x);
      std::uint32_t x1 = x0 + 1;
      const double fx = src_x - static_cast<double>(x0);
      if (x0 >= width) x0 = width - 1;
      if (x1 >= width) x1 = width - 1;
      const std::uint8_t* p00 = rgba + (static_cast<std::size_t>(y0) * width + x0) * 4;
      const std::uint8_t* p10 = rgba + (static_cast<std::size_t>(y0) * width + x1) * 4;
      const std::uint8_t* p01 = rgba + (static_cast<std::size_t>(y1) * width + x0) * 4;
      const std::uint8_t* p11 = rgba + (static_cast<std::size_t>(y1) * width + x1) * 4;
      for (int c = 0; c < 4; ++c) {
        const double v = (p00[c] * (1.0 - fx) + p10[c] * fx) * (1.0 - fy) +
                         (p01[c] * (1.0 - fx) + p11[c] * fx) * fy;
        row[x * 4 + c] = static_cast<std::uint8_t>(v + 0.5);
      }
    }
  }
  n.context->Unmap(n.staging[view_index], 0);
  ID3D11Texture2D* dst_tex =
      n.images[view_index][static_cast<std::size_t>(
          n.acquired_index[view_index])].texture;
  if (dst_tex == nullptr) return false;
  n.context->CopySubresourceRegion(dst_tex, 0, 0, 0, 0,
                                   n.staging[view_index], 0, nullptr);
  return true;
}

void RealOpenXRBackend::releaseSwapchainImage(std::uint32_t view_index) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || view_index >= view_count_) return;
  if (n.quad_enabled) {
    if (view_index == 0 && n.quad_acquired >= 0 &&
        n.quad_chain != XR_NULL_HANDLE) {
      XrSwapchainImageReleaseInfo info{};
      info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
      (void)xrReleaseSwapchainImage(n.quad_chain, &info);
      n.quad_acquired = -1;
    }
    n.acquired_index[view_index] = -1;
    return;
  }
  XrSwapchainImageReleaseInfo info{};
  info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
  (void)xrReleaseSwapchainImage(n.chains[view_index], &info);
  n.acquired_index[view_index] = -1;  // Upload requires a fresh acquire.
}

bool RealOpenXRBackend::endFrame(bool submitted) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || !frame_open_) return false;
  XrFrameEndInfo info{};
  info.type = XR_TYPE_FRAME_END_INFO;
  info.displayTime = last_timing_.predicted_display_time_ns;
  info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  // Quad mono (default): one image on a head-locked VIEW-space screen
  // 2.5 m ahead at eye level, ~90-degree horizontal extent, native source
  // aspect (no crop/stretch). The runtime renders each eye's view of the
  // quad natively -> correct convergence; the game camera is untouched.
  // MECVR_QUAD_SPACE=local selects the world-locked LOCAL pose instead;
  // MECVR_MONO_LAYER=projection selects the legacy path below.
  XrSpace quad_space = n.quad_local ? n.local : n.view;
  if (quad_space == XR_NULL_HANDLE) quad_space = n.local;
  if (submitted && n.quad_enabled && n.quad_chain != XR_NULL_HANDLE &&
      quad_space != XR_NULL_HANDLE && n.quad_w > 0 && n.quad_h > 0) {
    XrCompositionLayerQuad quad{};
    quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    quad.space = quad_space;
    quad.pose.orientation.x = 0.0f;
    quad.pose.orientation.y = 0.0f;
    quad.pose.orientation.z = 0.0f;
    quad.pose.orientation.w = 1.0f;
    quad.pose.position.x = 0.0f;
    quad.pose.position.y = n.quad_local ? 1.6f : 0.0f;
    quad.pose.position.z = -2.5f;
    quad.size.width = 5.0f;
    quad.size.height =
        5.0f * static_cast<float>(n.quad_h) / static_cast<float>(n.quad_w);
    quad.subImage.swapchain = n.quad_chain;
    quad.subImage.imageRect.offset.x = 0;
    quad.subImage.imageRect.offset.y = 0;
    quad.subImage.imageRect.extent.width = static_cast<std::int32_t>(n.quad_w);
    quad.subImage.imageRect.extent.height =
        static_cast<std::int32_t>(n.quad_h);
    quad.subImage.imageArrayIndex = 0;
    const XrCompositionLayerBaseHeader* quad_ptr =
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    info.layerCount = 1;
    info.layers = &quad_ptr;
    const bool qok = xrEndFrame(n.session, &info) == XR_SUCCESS;
    if (!qok) ++diagnostics_.end_failed;
    frame_open_ = false;
    return qok;
  }
  // M2B projection layer: the SAME uploaded image in both eyes (mono —
  // identical subimages, no per-eye offset; head-pose differences reach
  // the user only through the compositor's own reprojection).
  XrCompositionLayerProjection layer{};
  XrCompositionLayerProjectionView views[2] = {};
  const XrCompositionLayerBaseHeader* layer_ptrs[1] = {nullptr};
  if (submitted && n.last_raw_valid &&
      n.last_base_space != XR_NULL_HANDLE) {
    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    layer.space = n.last_base_space;
    layer.viewCount = 2;
    layer.views = views;
    for (int i = 0; i < 2; ++i) {
      views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
      views[i].pose = n.last_raw_views[i].pose;
      views[i].fov = n.last_raw_views[i].fov;
      views[i].subImage.swapchain = n.chains[i];
      views[i].subImage.imageRect.offset.x = 0;
      views[i].subImage.imageRect.offset.y = 0;
      views[i].subImage.imageRect.extent.width =
          static_cast<std::int32_t>(view_configs_[i].recommended_width);
      views[i].subImage.imageRect.extent.height =
          static_cast<std::int32_t>(view_configs_[i].recommended_height);
      views[i].subImage.imageArrayIndex = 0;
    }
    layer_ptrs[0] =
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    info.layerCount = 1;
    info.layers = layer_ptrs;
  } else {
    info.layerCount = 0;
    info.layers = nullptr;
  }
  const bool ok = xrEndFrame(n.session, &info) == XR_SUCCESS;
  if (!ok) ++diagnostics_.end_failed;
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
