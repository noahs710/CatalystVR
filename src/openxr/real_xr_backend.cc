#include "openxr/real_xr_backend.h"

// Real OpenXR backend (plan T8, M1B). Standalone session against the
// registered runtime (VDXR on this machine): own D3D11 device, dedicated
// caller-owned XR worker thread, graceful headset-absent degradation.

// NOTE: d3d11.h must come first: openxr_platform.h uses LUID,
// D3D_FEATURE_LEVEL, and ID3D11Device in its D3D11 structs.
#pragma warning(push, 3)
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
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

#include "render/shared_blit_math.h"

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

std::uint32_t RequestedXrResolution() {
  char value[32] = {};
  const DWORD length =
      GetEnvironmentVariableA("MECVR_XR_RESOLUTION", value, sizeof(value));
  if (length == 0 || length >= sizeof(value)) return 2048u;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0') return 2048u;
  if (parsed < 512ul) return 512u;
  if (parsed > 4096ul) return 4096u;
  return static_cast<std::uint32_t>(parsed);
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

bool HasExtension(const std::vector<::XrExtensionProperties>& properties,
                  const char* name) {
  if (name == nullptr) return false;
  for (const auto& property : properties) {
    if (std::strcmp(property.extensionName, name) == 0) return true;
  }
  return false;
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
  ID3D11Device1* device1 = nullptr;
  struct SharedSlot {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    IDXGIKeyedMutex* mutex = nullptr;
  } shared_slots[3];
  std::uint64_t shared_generation = 0;
  render::SharedCaptureRegistration shared_registration{};
  ID3D11VertexShader* shared_vs = nullptr;
  ID3D11PixelShader* shared_ps = nullptr;
  ID3D11SamplerState* shared_sampler = nullptr;
  ID3D11Buffer* shared_constants = nullptr;
  std::vector<ID3D11RenderTargetView*> shared_rtvs[2];
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
  XrActionSet action_set = XR_NULL_HANDLE;
  XrAction pose_action = XR_NULL_HANDLE;
  XrAction trigger_action = XR_NULL_HANDLE;
  XrAction squeeze_action = XR_NULL_HANDLE;
  XrAction thumbstick_action = XR_NULL_HANDLE;
  XrAction primary_action = XR_NULL_HANDLE;
  XrAction secondary_action = XR_NULL_HANDLE;
  XrAction menu_action = XR_NULL_HANDLE;
  XrPath hand_paths[2] = {XR_NULL_PATH, XR_NULL_PATH};
  XrSpace grip_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  ControllerState controller_cache[2] = {};
  bool actions_ready = false;
  bool body_extensions_enabled = false;
  bool full_body_supported = false;
  bool body_tracking_ready = false;
  XrBodyTrackerFB body_tracker = XR_NULL_HANDLE;
  PFN_xrCreateBodyTrackerFB create_body_tracker = nullptr;
  PFN_xrDestroyBodyTrackerFB destroy_body_tracker = nullptr;
  PFN_xrLocateBodyJointsFB locate_body_joints = nullptr;
  PFN_xrGetBodySkeletonFB get_body_skeleton = nullptr;
  std::array<::XrBodyJointLocationFB, XR_FULL_BODY_JOINT_COUNT_META>
      body_locations{};
  BodyTrackingSnapshot body_cache{};
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

BodyTrackingSnapshot RealOpenXRBackend::bodyTracking() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return native_->body_cache;
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

    std::uint32_t extension_count = 0;
    std::vector<::XrExtensionProperties> properties;
    if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count,
                                               nullptr) == XR_SUCCESS &&
        extension_count > 0) {
      properties.resize(extension_count);
      for (auto& property : properties)
        property.type = XR_TYPE_EXTENSION_PROPERTIES;
      (void)xrEnumerateInstanceExtensionProperties(
          nullptr, extension_count, &extension_count, properties.data());
    }
    std::vector<const char*> extensions = {"XR_KHR_D3D11_enable"};
    n.body_extensions_enabled =
        HasExtension(properties, XR_FB_BODY_TRACKING_EXTENSION_NAME) &&
        HasExtension(properties, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);
    if (n.body_extensions_enabled) {
      extensions.push_back(XR_FB_BODY_TRACKING_EXTENSION_NAME);
      extensions.push_back(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);
    }
    XrInstanceCreateInfo create{};
    create.type = XR_TYPE_INSTANCE_CREATE_INFO;
    create.applicationInfo = app;
    create.enabledExtensionCount =
        static_cast<std::uint32_t>(extensions.size());
    create.enabledExtensionNames = extensions.data();

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
    if (n.body_extensions_enabled) {
      (void)xrGetInstanceProcAddr(
          n.instance, "xrCreateBodyTrackerFB",
          reinterpret_cast<PFN_xrVoidFunction*>(&n.create_body_tracker));
      (void)xrGetInstanceProcAddr(
          n.instance, "xrDestroyBodyTrackerFB",
          reinterpret_cast<PFN_xrVoidFunction*>(&n.destroy_body_tracker));
      (void)xrGetInstanceProcAddr(
          n.instance, "xrLocateBodyJointsFB",
          reinterpret_cast<PFN_xrVoidFunction*>(&n.locate_body_joints));
      (void)xrGetInstanceProcAddr(
          n.instance, "xrGetBodySkeletonFB",
          reinterpret_cast<PFN_xrVoidFunction*>(&n.get_body_skeleton));
    }
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

  if (n.body_extensions_enabled) {
    XrSystemPropertiesBodyTrackingFullBodyMETA full_body{};
    full_body.type = XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META;
    XrSystemBodyTrackingPropertiesFB body{};
    body.type = XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB;
    body.next = &full_body;
    XrSystemProperties properties{};
    properties.type = XR_TYPE_SYSTEM_PROPERTIES;
    properties.next = &body;
    if (xrGetSystemProperties(n.instance, n.system, &properties) == XR_SUCCESS) {
      n.full_body_supported = body.supportsBodyTracking == XR_TRUE &&
                              full_body.supportsFullBodyTracking == XR_TRUE;
    }
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
    if (FAILED(n.device->QueryInterface(__uuidof(ID3D11Device1),
                                        reinterpret_cast<void**>(&n.device1))) ||
        n.device1 == nullptr) {
      d.failure_reason = "runtime adapter lacks ID3D11Device1 sharing";
      n.context->Release();
      n.context = nullptr;
      n.device->Release();
      n.device = nullptr;
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
      if (n.device1 != nullptr) {
        n.device1->Release();
        n.device1 = nullptr;
      }
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

  // 5b. Optional Meta full-body provider. A failed tracker creation is not a
  // session failure: procedural IK remains the deterministic fallback.
  if (n.body_extensions_enabled && n.full_body_supported &&
      n.create_body_tracker != nullptr && n.destroy_body_tracker != nullptr &&
      n.locate_body_joints != nullptr) {
    XrBodyTrackerCreateInfoFB info{};
    info.type = XR_TYPE_BODY_TRACKER_CREATE_INFO_FB;
    info.bodyJointSet = XR_BODY_JOINT_SET_FULL_BODY_META;
    if (n.create_body_tracker(n.session, &info, &n.body_tracker) == XR_SUCCESS &&
        n.body_tracker != XR_NULL_HANDLE) {
      n.body_tracking_ready = true;
    }
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

  // 6b. Native OpenXR input actions. This is deliberately optional: a
  // runtime may expose views but no controller interaction profile. In that
  // case the camera/session remains usable and controllerState stays neutral.
  {
    const auto path = [&](const char* text, XrPath* out) {
      return xrStringToPath(n.instance, text, out) == XR_SUCCESS;
    };
    const bool paths_ok = path("/user/hand/left", &n.hand_paths[0]) &&
                          path("/user/hand/right", &n.hand_paths[1]);
    XrActionSetCreateInfo set_info{};
    set_info.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    std::snprintf(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s",
                  "mecvr");
    std::snprintf(set_info.localizedActionSetName,
                  XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s", "MECVR");
    set_info.priority = 0;
    const auto make_action = [&](const char* name, const char* localized,
                                 XrActionType type, XrAction* out) {
      XrActionCreateInfo info{};
      info.type = XR_TYPE_ACTION_CREATE_INFO;
      std::snprintf(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
      std::snprintf(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
                    "%s", localized);
      info.actionType = type;
      info.countSubactionPaths = 2;
      info.subactionPaths = n.hand_paths;
      return xrCreateAction(n.action_set, &info, out) == XR_SUCCESS;
    };
    if (paths_ok && xrCreateActionSet(n.instance, &set_info, &n.action_set) ==
                        XR_SUCCESS &&
        make_action("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT,
                    &n.pose_action) &&
        make_action("trigger", "Trigger", XR_ACTION_TYPE_BOOLEAN_INPUT,
                    &n.trigger_action) &&
        make_action("squeeze", "Squeeze", XR_ACTION_TYPE_BOOLEAN_INPUT,
                    &n.squeeze_action) &&
        make_action("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT,
                    &n.thumbstick_action) &&
        make_action("primary", "Primary", XR_ACTION_TYPE_BOOLEAN_INPUT,
                    &n.primary_action) &&
        make_action("secondary", "Secondary", XR_ACTION_TYPE_BOOLEAN_INPUT,
                    &n.secondary_action) &&
        make_action("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT,
                    &n.menu_action)) {
      bool spaces_ok = true;
      for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo info{};
        info.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        info.action = n.pose_action;
        info.subactionPath = n.hand_paths[hand];
        info.poseInActionSpace.orientation.w = 1.0f;
        if (xrCreateActionSpace(n.session, &info, &n.grip_spaces[hand]) !=
            XR_SUCCESS) {
          spaces_ok = false;
          break;
        }
      }
      n.actions_ready = spaces_ok;
      if (n.actions_ready) {
        // Prefer the common Oculus Touch profile and keep this advisory:
        // runtimes may select a different profile at session start. The
        // action set itself remains valid even when this suggestion is not.
        XrPath profile = XR_NULL_PATH;
        XrActionSuggestedBinding bindings[12] = {};
        const char* binding_paths[12] = {
            "/user/hand/left/input/grip/pose",
            "/user/hand/right/input/grip/pose",
            "/user/hand/left/input/trigger/click",
            "/user/hand/right/input/trigger/click",
            "/user/hand/left/input/squeeze/click",
            "/user/hand/right/input/squeeze/click",
            "/user/hand/left/input/thumbstick",
            "/user/hand/right/input/thumbstick",
            "/user/hand/left/input/x/click",
            "/user/hand/right/input/a/click",
            "/user/hand/left/input/y/click",
            "/user/hand/right/input/b/click"};
        const XrAction actions[12] = {
            n.pose_action,       n.pose_action,       n.trigger_action,
            n.trigger_action,    n.squeeze_action,    n.squeeze_action,
            n.thumbstick_action, n.thumbstick_action, n.primary_action,
            n.primary_action,    n.secondary_action,  n.secondary_action};
        bool binding_paths_ok =
            xrStringToPath(n.instance,
                           "/interaction_profiles/oculus/touch_controller",
                           &profile) == XR_SUCCESS;
        for (int i = 0; i < 12 && binding_paths_ok; ++i) {
          bindings[i].action = actions[i];
          binding_paths_ok = xrStringToPath(n.instance, binding_paths[i],
                                            &bindings[i].binding) == XR_SUCCESS;
        }
        if (binding_paths_ok) {
          XrInteractionProfileSuggestedBinding suggest{};
          suggest.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
          suggest.interactionProfile = profile;
          suggest.suggestedBindings = bindings;
          suggest.countSuggestedBindings = 12;
          (void)xrSuggestInteractionProfileBindings(n.instance, &suggest);
        }
        XrSessionActionSetsAttachInfo attach{};
        attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
        attach.countActionSets = 1;
        attach.actionSets = &n.action_set;
        n.actions_ready = xrAttachSessionActionSets(n.session, &attach) ==
                          XR_SUCCESS;
      }
    }
    if (!n.actions_ready) {
      for (XrSpace& space : n.grip_spaces) {
        if (space != XR_NULL_HANDLE) xrDestroySpace(space);
        space = XR_NULL_HANDLE;
      }
      const XrAction actions[] = {n.pose_action, n.trigger_action,
                                  n.squeeze_action, n.thumbstick_action,
                                  n.primary_action, n.secondary_action,
                                  n.menu_action};
      for (XrAction action : actions) {
        if (action != XR_NULL_HANDLE) xrDestroyAction(action);
      }
      if (n.action_set != XR_NULL_HANDLE) xrDestroyActionSet(n.action_set);
      n.action_set = XR_NULL_HANDLE;
      n.pose_action = n.trigger_action = n.squeeze_action = XR_NULL_HANDLE;
      n.thumbstick_action = n.primary_action = n.secondary_action =
          n.menu_action = XR_NULL_HANDLE;
    }
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
      const SwapchainSize selected = SelectSwapchainSize(
          view_configs_[i], RequestedXrResolution());
      view_configs_[i].swapchain_width = selected.width;
      view_configs_[i].swapchain_height = selected.height;
    }
    char resolution_debug[256] = {};
    std::snprintf(
        resolution_debug, sizeof(resolution_debug),
        "[XR] eye target requested=%u left=%ux%u right=%ux%u "
        "recommended=%ux%u max=%ux%u\n",
        RequestedXrResolution(), view_configs_[0].swapchain_width,
        view_configs_[0].swapchain_height, view_configs_[1].swapchain_width,
        view_configs_[1].swapchain_height, view_configs_[0].recommended_width,
        view_configs_[0].recommended_height, view_configs_[0].max_width,
        view_configs_[0].max_height);
    OutputDebugStringA(resolution_debug);

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
      info.width = view_configs_[i].swapchain_width;
      info.height = view_configs_[i].swapchain_height;
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
  for (int eye = 0; eye < 2; ++eye) {
    for (ID3D11RenderTargetView*& rtv : n.shared_rtvs[eye]) {
      if (rtv != nullptr) rtv->Release();
      rtv = nullptr;
    }
    n.shared_rtvs[eye].clear();
  }
  for (auto& slot : n.shared_slots) {
    if (slot.mutex != nullptr) slot.mutex->Release();
    if (slot.srv != nullptr) slot.srv->Release();
    if (slot.texture != nullptr) slot.texture->Release();
    slot = {};
  }
  if (n.shared_constants != nullptr) n.shared_constants->Release();
  if (n.shared_sampler != nullptr) n.shared_sampler->Release();
  if (n.shared_ps != nullptr) n.shared_ps->Release();
  if (n.shared_vs != nullptr) n.shared_vs->Release();
  n.shared_constants = nullptr;
  n.shared_sampler = nullptr;
  n.shared_ps = nullptr;
  n.shared_vs = nullptr;
  n.shared_generation = 0;
  n.shared_registration = {};
  diagnostics_.gpu_transport_active = false;
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
  if (n.body_tracker != XR_NULL_HANDLE && n.destroy_body_tracker != nullptr) {
    (void)n.destroy_body_tracker(n.body_tracker);
  }
  n.body_tracker = XR_NULL_HANDLE;
  n.body_tracking_ready = false;
  n.body_cache = {};
  for (XrSpace& space : n.grip_spaces) {
    if (space != XR_NULL_HANDLE) xrDestroySpace(space);
    space = XR_NULL_HANDLE;
  }
  const XrAction actions[] = {n.pose_action, n.trigger_action, n.squeeze_action,
                              n.thumbstick_action, n.primary_action,
                              n.secondary_action, n.menu_action};
  for (XrAction action : actions) {
    if (action != XR_NULL_HANDLE) xrDestroyAction(action);
  }
  if (n.action_set != XR_NULL_HANDLE) xrDestroyActionSet(n.action_set);
  n.action_set = XR_NULL_HANDLE;
  n.pose_action = n.trigger_action = n.squeeze_action = XR_NULL_HANDLE;
  n.thumbstick_action = n.primary_action = n.secondary_action =
      n.menu_action = XR_NULL_HANDLE;
  n.actions_ready = false;
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
  if (n.device1 != nullptr) {
    n.device1->Release();
    n.device1 = nullptr;
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
  if (n.body_tracking_ready) {
    std::array<int, kBodyTrackingJointCount> mapping = {
        XR_FULL_BODY_JOINT_ROOT_META,
        XR_FULL_BODY_JOINT_HIPS_META,
        XR_FULL_BODY_JOINT_CHEST_META,
        XR_FULL_BODY_JOINT_NECK_META,
        XR_FULL_BODY_JOINT_HEAD_META,
        XR_FULL_BODY_JOINT_LEFT_SHOULDER_META,
        XR_FULL_BODY_JOINT_LEFT_ARM_LOWER_META,
        XR_FULL_BODY_JOINT_LEFT_HAND_WRIST_META,
        XR_FULL_BODY_JOINT_LEFT_HAND_PALM_META,
        XR_FULL_BODY_JOINT_RIGHT_SHOULDER_META,
        XR_FULL_BODY_JOINT_RIGHT_ARM_LOWER_META,
        XR_FULL_BODY_JOINT_RIGHT_HAND_WRIST_META,
        XR_FULL_BODY_JOINT_RIGHT_HAND_PALM_META,
        XR_FULL_BODY_JOINT_LEFT_UPPER_LEG_META,
        XR_FULL_BODY_JOINT_LEFT_LOWER_LEG_META,
        XR_FULL_BODY_JOINT_LEFT_FOOT_ANKLE_META,
        XR_FULL_BODY_JOINT_LEFT_FOOT_BALL_META,
        XR_FULL_BODY_JOINT_RIGHT_UPPER_LEG_META,
        XR_FULL_BODY_JOINT_RIGHT_LOWER_LEG_META,
        XR_FULL_BODY_JOINT_RIGHT_FOOT_ANKLE_META,
        XR_FULL_BODY_JOINT_RIGHT_FOOT_BALL_META};
    XrBodyJointsLocateInfoFB locate_body{};
    locate_body.type = XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB;
    locate_body.baseSpace = n.local;
    locate_body.time = last_timing_.predicted_display_time_ns;
    XrBodyJointLocationsFB locations{};
    locations.type = XR_TYPE_BODY_JOINT_LOCATIONS_FB;
    locations.jointCount = XR_FULL_BODY_JOINT_COUNT_META;
    locations.jointLocations = n.body_locations.data();
    const XrResult body_result =
        n.locate_body_joints(n.body_tracker, &locate_body, &locations);
    BodyTrackingSnapshot snapshot{};
    snapshot.sample_time_ns = locations.time;
    snapshot.confidence = locations.confidence;
    snapshot.active = body_result == XR_SUCCESS && locations.isActive == XR_TRUE &&
                      locations.jointCount == XR_FULL_BODY_JOINT_COUNT_META;
    if (snapshot.active) {
      for (std::size_t i = 0; i < mapping.size(); ++i) {
        const auto& joint = n.body_locations[static_cast<std::size_t>(mapping[i])];
        const auto required = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((joint.locationFlags & required) != required) continue;
        snapshot.joints[i] = ToSeamPose(joint.pose);
        snapshot.valid_mask |= (1u << i);
      }
    }
    n.body_cache = snapshot;
  }
  if (n.actions_ready) {
    XrActiveActionSet active{};
    active.actionSet = n.action_set;
    XrActionsSyncInfo sync{};
    sync.type = XR_TYPE_ACTIONS_SYNC_INFO;
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (xrSyncActions(n.session, &sync) == XR_SUCCESS) {
      for (int hand = 0; hand < 2; ++hand) {
        ControllerState controller;
        controller.grip_pose = IdentityPose();
        const XrPath subaction = n.hand_paths[hand];
        const auto read_bool = [&](XrAction action) {
          XrActionStateGetInfo get{};
          get.type = XR_TYPE_ACTION_STATE_GET_INFO;
          get.action = action;
          get.subactionPath = subaction;
          XrActionStateBoolean value{};
          value.type = XR_TYPE_ACTION_STATE_BOOLEAN;
          return xrGetActionStateBoolean(n.session, &get, &value) == XR_SUCCESS &&
                 value.isActive == XR_TRUE && value.currentState == XR_TRUE;
        };
        const auto read_float = [&](XrAction action) {
          XrActionStateGetInfo get{};
          get.type = XR_TYPE_ACTION_STATE_GET_INFO;
          get.action = action;
          get.subactionPath = subaction;
          XrActionStateFloat value{};
          value.type = XR_TYPE_ACTION_STATE_FLOAT;
          if (xrGetActionStateFloat(n.session, &get, &value) != XR_SUCCESS ||
              value.isActive != XR_TRUE) {
            return 0.0f;
          }
          return std::clamp(value.currentState, 0.0f, 1.0f);
        };
        controller.trigger_value = read_float(n.trigger_action);
        controller.squeeze_value = read_float(n.squeeze_action);
        if (controller.trigger_value > 0.15f || read_bool(n.trigger_action))
          controller.buttons |= kButtonTrigger;
        if (controller.squeeze_value > 0.15f || read_bool(n.squeeze_action))
          controller.buttons |= kButtonSqueeze;
        if (read_bool(n.primary_action)) controller.buttons |= kButtonPrimary;
        if (read_bool(n.secondary_action)) controller.buttons |= kButtonSecondary;
        if (read_bool(n.menu_action)) controller.buttons |= kButtonMenu;

        XrActionStateGetInfo axis_get{};
        axis_get.type = XR_TYPE_ACTION_STATE_GET_INFO;
        axis_get.action = n.thumbstick_action;
        axis_get.subactionPath = subaction;
        XrActionStateVector2f axis{};
        axis.type = XR_TYPE_ACTION_STATE_VECTOR2F;
        if (xrGetActionStateVector2f(n.session, &axis_get, &axis) == XR_SUCCESS &&
            axis.isActive == XR_TRUE) {
          controller.thumbstick_x = axis.currentState.x;
          controller.thumbstick_y = axis.currentState.y;
          if (std::fabs(controller.thumbstick_x) > 0.15f ||
              std::fabs(controller.thumbstick_y) > 0.15f) {
            controller.buttons |= kButtonThumbstick;
          }
        }

        XrSpaceLocation location{};
        location.type = XR_TYPE_SPACE_LOCATION;
        if (n.grip_spaces[hand] != XR_NULL_HANDLE &&
            xrLocateSpace(n.grip_spaces[hand], n.local,
                          last_timing_.predicted_display_time_ns,
                          &location) == XR_SUCCESS &&
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) !=
                0) {
          controller.grip_pose = ToSeamPose(location.pose);
          controller.pose_valid = true;
        }
        n.controller_cache[hand] = controller;
      }
    }
  }
  return out;
}

// Quad mono presentation (M2B convergence fix). Identical pixels through
// two IPD-offset projection frustums disagree per eye (dizzying); a single
// compositor quad lets the runtime render each eye's view of ONE image
// natively — correct convergence with mono content, no stereo work.
// The quad is a diagnostics/cinema fallback only. Immersive camera mode uses
// projection layers by default; MECVR_MONO_LAYER=quad explicitly requests the
// theatre presentation. Helpers assume mutex_ is held.
bool RealOpenXRBackend::QuadWanted() {
  char v[32] = {};
  const DWORD n = GetEnvironmentVariableA("MECVR_MONO_LAYER", v, sizeof(v));
  if (n == 0 || _stricmp(v, "quad") != 0) return false;
  // Theatre mode is a deliberate diagnostic path. A second opt-in prevents
  // stale process state from replacing immersive projection unexpectedly.
  char allow[8] = {};
  const DWORD allow_len =
      GetEnvironmentVariableA("MECVR_ALLOW_THEATRE", allow, sizeof(allow));
  return allow_len > 0 &&
         (allow[0] == '1' || allow[0] == 'y' || allow[0] == 'Y');
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

bool RealOpenXRBackend::enableStereoProjection() {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load()) return false;
  // A stereo frame must never be routed through the single-image quad.
  // Destroying it before the next acquire also prevents an in-flight alias.
  if (n.quad_enabled) DestroyQuadLocked(n);
  n.quad_tried = true;
  diagnostics_.mono_layer = "projection (stereo)";
  diagnostics_.mono_space = "n-a";
  return true;
}

bool RealOpenXRBackend::registerSharedCapture(
    const render::SharedCaptureRegistration& registration) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || n.device == nullptr || n.device1 == nullptr ||
      !render::IsValidSharedCaptureRegistration(registration)) {
    ++diagnostics_.gpu_registration_failed;
    return false;
  }
  if (n.shared_generation == registration.generation) return true;

  IDXGIDevice* dxgi = nullptr;
  IDXGIAdapter* adapter = nullptr;
  DXGI_ADAPTER_DESC desc{};
  const bool luid_ok =
      SUCCEEDED(n.device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(&dxgi))) &&
      dxgi != nullptr && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
      adapter != nullptr && SUCCEEDED(adapter->GetDesc(&desc)) &&
      desc.AdapterLuid.LowPart == registration.luid.low &&
      desc.AdapterLuid.HighPart == registration.luid.high;
  if (adapter != nullptr) adapter->Release();
  if (dxgi != nullptr) dxgi->Release();
  if (!luid_ok) {
    ++diagnostics_.gpu_registration_failed;
    return false;
  }

  // A new generation is accepted only after the worker has released every
  // object from the previous generation.
  for (int eye = 0; eye < 2; ++eye) {
    for (auto*& rtv : n.shared_rtvs[eye]) if (rtv != nullptr) rtv->Release();
    n.shared_rtvs[eye].clear();
  }
  for (auto& slot : n.shared_slots) {
    if (slot.mutex != nullptr) slot.mutex->Release();
    if (slot.srv != nullptr) slot.srv->Release();
    if (slot.texture != nullptr) slot.texture->Release();
    slot = {};
  }
  n.shared_generation = 0;
  n.shared_registration = {};

  diagnostics_.gpu_transport_active = false;

  for (std::uint32_t i = 0; i < registration.slot_count; ++i) {
    HANDLE handle = reinterpret_cast<HANDLE>(
        static_cast<std::uintptr_t>(registration.handles[i]));
    auto& slot = n.shared_slots[i];
    if (FAILED(n.device1->OpenSharedResource1(
            handle, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&slot.texture))) ||
        slot.texture == nullptr ||
        FAILED(n.device->CreateShaderResourceView(slot.texture, nullptr,
                                                   &slot.srv)) ||
        FAILED(slot.texture->QueryInterface(__uuidof(IDXGIKeyedMutex),
                                             reinterpret_cast<void**>(
                                                 &slot.mutex)))) {
      ++diagnostics_.gpu_registration_failed;
      return false;
    }
  }

  for (int eye = 0; eye < 2; ++eye) {
    n.shared_rtvs[eye].resize(n.images[eye].size(), nullptr);
    for (std::size_t i = 0; i < n.images[eye].size(); ++i) {
      if (n.images[eye][i].texture == nullptr ||
          FAILED(n.device->CreateRenderTargetView(
              n.images[eye][i].texture, nullptr,
              &n.shared_rtvs[eye][i]))) {
        ++diagnostics_.gpu_registration_failed;
        return false;
      }
    }
  }
  n.shared_registration = registration;
  n.shared_generation = registration.generation;
  diagnostics_.gpu_transport_active = true;
  return true;
}

bool RealOpenXRBackend::submitSharedFrame(
    const render::SharedCaptureFrame& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (!running_.load() || n.context == nullptr || frame.slot >= 3 ||
      !render::IsValidSharedCaptureFrame(frame, n.shared_registration) ||
      n.shared_slots[frame.slot].mutex == nullptr ||
      n.shared_slots[frame.slot].srv == nullptr) {
    return false;
  }

  if (n.shared_vs == nullptr || n.shared_ps == nullptr) {
    static constexpr char shader[] =
        "cbuffer C:register(b0){float4 crop;}"
        "struct O{float4 p:SV_POSITION;float2 u:TEXCOORD0;};"
        "O vs(uint i:SV_VertexID){O o;float2 q=float2((i<<1)&2,i&2);"
        "o.u=q;o.p=float4(q*float2(2,-2)+float2(-1,1),0,1);return o;}"
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 ps(O i):SV_TARGET{return t.SampleLevel(s,crop.xy+i.u*crop.zw,0);}";
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    const HRESULT vh = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr,
                                  nullptr, "vs", "vs_5_0", 0, 0, &vs,
                                  nullptr);
    const HRESULT ph = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr,
                                  nullptr, "ps", "ps_5_0", 0, 0, &ps,
                                  nullptr);
    const bool compiled = SUCCEEDED(vh) && SUCCEEDED(ph) && vs != nullptr &&
                          ps != nullptr;
    if (!compiled ||
        FAILED(n.device->CreateVertexShader(vs->GetBufferPointer(),
                                             vs->GetBufferSize(), nullptr,
                                             &n.shared_vs)) ||
        FAILED(n.device->CreatePixelShader(ps->GetBufferPointer(),
                                            ps->GetBufferSize(), nullptr,
                                            &n.shared_ps))) {
      if (vs != nullptr) vs->Release();
      if (ps != nullptr) ps->Release();
      return false;
    }
    vs->Release();
    ps->Release();
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(n.device->CreateSamplerState(&sd, &n.shared_sampler)) ||
        FAILED(n.device->CreateBuffer(&bd, nullptr, &n.shared_constants))) {
      return false;
    }
  }

  auto& slot = n.shared_slots[frame.slot];
  if (slot.mutex->AcquireSync(1, 0) != S_OK) {
    ++diagnostics_.gpu_acquire_timeout;
    return false;
  }
  bool ok = true;
  for (std::uint32_t eye = 0; eye < 2 && ok; ++eye) {
    const std::int64_t acquired = n.acquired_index[eye];
    if (acquired < 0 || static_cast<std::size_t>(acquired) >=
                            n.shared_rtvs[eye].size()) {
      ok = false;
      break;
    }
    ID3D11Texture2D* target =
        n.images[eye][static_cast<std::size_t>(acquired)].texture;
    D3D11_TEXTURE2D_DESC target_desc{};
    target->GetDesc(&target_desc);
    const auto crop = render::CenterCropUv(frame.width, frame.height,
                                            target_desc.Width,
                                            target_desc.Height);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!crop.valid || FAILED(n.context->Map(n.shared_constants, 0,
                                             D3D11_MAP_WRITE_DISCARD, 0,
                                             &mapped))) {
      ok = false;
      break;
    }
    const float values[4] = {crop.origin_x, crop.origin_y, crop.extent_x,
                             crop.extent_y};
    std::memcpy(mapped.pData, values, sizeof(values));
    n.context->Unmap(n.shared_constants, 0);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(target_desc.Width);
    viewport.Height = static_cast<float>(target_desc.Height);
    viewport.MaxDepth = 1.0f;
    ID3D11RenderTargetView* rtv =
        n.shared_rtvs[eye][static_cast<std::size_t>(acquired)];
    n.context->OMSetRenderTargets(1, &rtv, nullptr);
    n.context->RSSetViewports(1, &viewport);
    n.context->IASetInputLayout(nullptr);
    n.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    n.context->VSSetShader(n.shared_vs, nullptr, 0);
    n.context->PSSetShader(n.shared_ps, nullptr, 0);
    n.context->PSSetShaderResources(0, 1, &slot.srv);
    n.context->PSSetSamplers(0, 1, &n.shared_sampler);
    n.context->PSSetConstantBuffers(0, 1, &n.shared_constants);
    n.context->Draw(3, 0);
    ID3D11ShaderResourceView* no_srv = nullptr;
    n.context->PSSetShaderResources(0, 1, &no_srv);
  }
  ID3D11RenderTargetView* no_rtv = nullptr;
  n.context->OMSetRenderTargets(1, &no_rtv, nullptr);
  slot.mutex->ReleaseSync(0);
  if (ok) ++diagnostics_.gpu_frames_submitted;
  return ok;
}

void RealOpenXRBackend::unregisterSharedCapture(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  Native& n = *native_;
  if (generation != 0 && generation != n.shared_generation) return;
  for (int eye = 0; eye < 2; ++eye) {
    for (auto*& rtv : n.shared_rtvs[eye]) if (rtv != nullptr) rtv->Release();
    n.shared_rtvs[eye].clear();
  }
  for (auto& slot : n.shared_slots) {
    if (slot.mutex != nullptr) slot.mutex->Release();
    if (slot.srv != nullptr) slot.srv->Release();
    if (slot.texture != nullptr) slot.texture->Release();
    slot = {};
  }
  n.shared_generation = 0;
  n.shared_registration = {};
  diagnostics_.gpu_transport_active = false;
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
  const std::uint32_t dst_w = view_configs_[view_index].swapchain_width;
  const std::uint32_t dst_h = view_configs_[view_index].swapchain_height;
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
  // Use the same centered crop as the shared GPU compositor. The runtime
  // owns the destination dimensions, but a desktop 16:9 source must fill the
  // immersive eye texture rather than become a black-bar theatre panel.
  const auto crop = render::CenterCropUv(width, height, dst_w, dst_h);
  if (!crop.valid) return false;
  const double crop_left = static_cast<double>(crop.origin_x) * width;
  const double crop_top = static_cast<double>(crop.origin_y) * height;
  const double crop_width = static_cast<double>(crop.extent_x) * width;
  const double crop_height = static_cast<double>(crop.extent_y) * height;
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(n.context->Map(n.staging[view_index], 0, D3D11_MAP_WRITE_DISCARD,
                            0, &mapped))) {
    return false;
  }
  // Nearest sampling keeps this fallback bounded. Every destination pixel is
  // filled from the centered crop, so no stale or black letterbox region can
  // leak into the eye image.
  auto* dst = static_cast<std::uint8_t*>(mapped.pData);
  for (std::uint32_t y = 0; y < dst_h; ++y) {
    const std::uint32_t sy = (std::min)(
        height - 1,
        static_cast<std::uint32_t>(
            crop_top + (static_cast<double>(y) + 0.5) * crop_height / dst_h));
    std::uint8_t* row =
        dst + static_cast<std::size_t>(y) * mapped.RowPitch;
    const std::uint8_t* source_row =
        rgba + static_cast<std::size_t>(sy) * width * 4;
    for (std::uint32_t x = 0; x < dst_w; ++x) {
      const std::uint32_t sx = (std::min)(
          width - 1,
          static_cast<std::uint32_t>(
              crop_left + (static_cast<double>(x) + 0.5) * crop_width / dst_w));
      std::memcpy(row + static_cast<std::size_t>(x) * 4,
                  source_row + static_cast<std::size_t>(sx) * 4, 4);
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
  // Theatre/mono is an explicit diagnostic path. The normal public path is
  // the two-eye projection layer below; no quad is created by default.
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
  // Immersive projection layer. Stereo frames upload independent eye images;
  // mono fallback may still use identical pixels, but the runtime receives
  // native per-eye poses and asymmetric FOVs.
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
          static_cast<std::int32_t>(view_configs_[i].swapchain_width);
      views[i].subImage.imageRect.extent.height =
          static_cast<std::int32_t>(view_configs_[i].swapchain_height);
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
  std::lock_guard<std::mutex> lock(mutex_);
  const int index = hand == Hand::kLeft ? 0 : 1;
  return native_->controller_cache[index];
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
