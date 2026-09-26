// T10G integrated D3D11 adapter/LUID gate (plan Key Decisions 1+10).
//
// Compares the recorded Catalyst adapter (OBSERVER_M0C2.md: AMD Radeon
// RX 9060 XT, vendor 0x1002, device 0x7590, LUID 0x00000000:0x0001a699,
// game D3D11 feature level 11.1) against the live DXGI enumeration and
// xrGetD3D11GraphicsRequirementsKHR from the active runtime (VDXR).
//
// Verdicts (also printed as T10G_GATE_VERDICT for the evidence record):
//   PASS — required LUID matches the recorded Catalyst LUID, the
//          runtime min feature level is satisfiable, and a session on
//          the matched adapter creates (then destroyed). Exit 0.
//   FAIL — LUID mismatch (both sides logged + plain-language note), or
//          any bring-up step on the matched path fails. Exit 1.
//   OPEN — headset absent (xrGetSystem -> XR_ERROR_FORM_FACTOR_
//          UNAVAILABLE, as seen before): nothing to gate against, gate
//          stays open honestly. Exit 2. Never faked to PASS.
//
// No game process, no hooks, no camera/stereo/gameplay code (STOP S3).

#pragma warning(push, 3)
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma warning(pop)

#define XR_USE_GRAPHICS_API_D3D11
#pragma warning(push, 3)
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#pragma warning(pop)

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int g_verdict_exit = 2;  // Default OPEN unless proven otherwise.

void Verdict(const char* verdict, int exit_code) {
  std::cout << "T10G_GATE_VERDICT: " << verdict << "\n";
  g_verdict_exit = exit_code;
}

// Recorded Catalyst facts (docs/OBSERVER_M0C2.md, retail 1.0.3.47248).
constexpr DWORD kGameLuidLow = 0x0001a699u;
constexpr LONG kGameLuidHigh = 0x00000000;
constexpr UINT kGameVendorId = 0x1002u;
constexpr UINT kGameDeviceId = 0x7590u;
constexpr D3D_FEATURE_LEVEL kGameFeatureLevel = D3D_FEATURE_LEVEL_11_1;

const char* XrResultName(XrResult r) {
  switch (r) {
    case XR_SUCCESS:
      return "XR_SUCCESS";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
      return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED:
      return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    case XR_ERROR_API_VERSION_UNSUPPORTED:
      return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_RUNTIME_FAILURE:
      return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_INITIALIZATION_FAILED:
      return "XR_ERROR_INITIALIZATION_FAILED";
    default:
      return "XR_OTHER";
  }
}

const char* FeatureLevelName(D3D_FEATURE_LEVEL level) {
  switch (level) {
    case D3D_FEATURE_LEVEL_11_1:
      return "11.1";
    case D3D_FEATURE_LEVEL_11_0:
      return "11.0";
    case D3D_FEATURE_LEVEL_10_1:
      return "10.1";
    case D3D_FEATURE_LEVEL_10_0:
      return "10.0";
    default:
      return "other";
  }
}

}  // namespace

int main() {
  std::cout << "T10G adapter/LUID gate\n";
  std::cout << "recorded catalyst: AMD Radeon RX 9060 XT vendor=0x"
            << std::hex << kGameVendorId << " device=0x" << kGameDeviceId
            << std::dec << " LUID=0x" << std::hex
            << static_cast<std::uint32_t>(kGameLuidHigh) << ":0x"
            << kGameLuidLow << std::dec << " FL="
            << FeatureLevelName(kGameFeatureLevel) << "\n";

  // ---- Step 1: enumerate local DXGI adapters/LUIDs ---------------------
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void**>(&factory))) ||
      factory == nullptr) {
    std::cout << "FAIL: CreateDXGIFactory1 failed\n";
    Verdict("FAIL", 1);
    return g_verdict_exit;
  }
  bool game_adapter_present = false;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) != S_OK || adapter == nullptr)
      break;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      char narrow[128] = {};
      for (int c = 0; c < 127 && desc.Description[c] != L'\0'; ++c)
        narrow[c] = static_cast<char>(desc.Description[c]);
      std::cout << "local adapter[" << i << "]: \"" << narrow
                << "\" vendor=0x" << std::hex << desc.VendorId
                << " device=0x" << desc.DeviceId << std::dec << " LUID=0x"
                << std::hex
                << static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)
                << ":0x" << desc.AdapterLuid.LowPart << std::dec << "\n";
      if (desc.AdapterLuid.LowPart == static_cast<DWORD>(kGameLuidLow) &&
          desc.AdapterLuid.HighPart == kGameLuidHigh)
        game_adapter_present = true;
    }
    adapter->Release();
  }
  std::cout << "recorded catalyst adapter "
            << (game_adapter_present ? "PRESENT" : "ABSENT")
            << " in local enumeration\n";

  // ---- Step 2: OpenXR instance against VDXR -----------------------------
  XrInstance instance = XR_NULL_HANDLE;
  {
    XrApplicationInfo app{};
    const char* name = "MECVR-T10G";
    for (int i = 0; i < 128 && name[i] != '\0'; ++i)
      app.applicationName[i] = name[i];
    app.applicationVersion = 1;
    app.apiVersion = XR_API_VERSION_1_0;  // VDXR rejects 1.1 (OPENXR_M1B).
    const char* extensions[] = {"XR_KHR_D3D11_enable"};
    XrInstanceCreateInfo create{};
    create.type = XR_TYPE_INSTANCE_CREATE_INFO;
    create.applicationInfo = app;
    create.enabledExtensionCount = 1;
    create.enabledExtensionNames = extensions;
    const XrResult r = xrCreateInstance(&create, &instance);
    if (r != XR_SUCCESS || instance == XR_NULL_HANDLE) {
      std::cout << "FAIL: xrCreateInstance failed: " << XrResultName(r)
                << "\n";
      factory->Release();
      Verdict("FAIL", 1);
      return g_verdict_exit;
    }
    XrInstanceProperties props{};
    props.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (xrGetInstanceProperties(instance, &props) == XR_SUCCESS) {
      const std::uint64_t v =
          static_cast<std::uint64_t>(props.runtimeVersion);
      std::cout << "runtime=\"" << props.runtimeName << "\" v"
                << ((v >> 48) & 0xffffu) << "." << ((v >> 32) & 0xffffu)
                << "." << (v & 0xffffffffu) << "\n";
    }
  }

  // ---- Step 3: HMD system (headset-absent -> OPEN, not PASS) ------------
  XrSystemId system = XR_NULL_SYSTEM_ID;
  {
    XrSystemGetInfo info{};
    info.type = XR_TYPE_SYSTEM_GET_INFO;
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult r = xrGetSystem(instance, &info, &system);
    if (r != XR_SUCCESS) {
      std::cout << "xrGetSystem (HMD) -> " << XrResultName(r) << "\n";
      xrDestroyInstance(instance);
      factory->Release();
      if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        std::cout << "OPEN: headset absent/unreachable; no runtime LUID "
                     "to gate against. Gate stays OPEN (not PASS).\n";
        Verdict("OPEN", 2);
      } else {
        std::cout << "FAIL: unexpected xrGetSystem failure.\n";
        Verdict("FAIL", 1);
      }
      return g_verdict_exit;
    }
  }
  std::cout << "system acquired: headset reachable, gating...\n";

  // ---- Step 4: runtime D3D11 requirements --------------------------------
  LUID required_luid{};
  D3D_FEATURE_LEVEL min_level = D3D_FEATURE_LEVEL_11_0;
  {
    PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
    (void)xrGetInstanceProcAddr(
        instance, "xrGetD3D11GraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
    if (get_requirements == nullptr) {
      std::cout << "FAIL: xrGetD3D11GraphicsRequirementsKHR unavailable\n";
      xrDestroyInstance(instance);
      factory->Release();
      Verdict("FAIL", 1);
      return g_verdict_exit;
    }
    XrGraphicsRequirementsD3D11KHR req{};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    const XrResult r = get_requirements(instance, system, &req);
    if (r != XR_SUCCESS) {
      std::cout << "FAIL: requirements query failed: " << XrResultName(r)
                << "\n";
      xrDestroyInstance(instance);
      factory->Release();
      Verdict("FAIL", 1);
      return g_verdict_exit;
    }
    required_luid = req.adapterLuid;
    min_level = req.minFeatureLevel;
    std::cout << "runtime requires: LUID=0x" << std::hex
              << static_cast<std::uint32_t>(required_luid.HighPart) << ":0x"
              << required_luid.LowPart << std::dec
              << " minFL=" << FeatureLevelName(min_level) << "\n";
  }

  // ---- Step 5: gate comparison -------------------------------------------
  const bool luid_match =
      required_luid.LowPart == static_cast<DWORD>(kGameLuidLow) &&
      required_luid.HighPart == kGameLuidHigh;
  const bool level_ok = kGameFeatureLevel >= min_level;
  std::cout << "gate: LUID " << (luid_match ? "MATCH" : "MISMATCH")
            << ", feature level " << (level_ok ? "OK" : "TOO LOW")
            << " (game 11.1 vs runtime min "
            << FeatureLevelName(min_level) << ")\n";
  if (!luid_match || !level_ok) {
    std::cout << "FAIL: GPU mismatch — Catalyst renders on LUID 0x"
              << std::hex << static_cast<std::uint32_t>(kGameLuidHigh)
              << ":0x" << kGameLuidLow << std::dec
              << " but the XR runtime requires LUID 0x" << std::hex
              << static_cast<std::uint32_t>(required_luid.HighPart) << ":0x"
              << required_luid.LowPart << std::dec << ".\n"
              << "Plain language: the game and the headset want different "
                 "GPUs, and Sub-project 1 does not copy frames across "
                 "GPUs, so the integrated path cannot proceed on this "
                 "machine configuration.\n";
    xrDestroyInstance(instance);
    factory->Release();
    Verdict("FAIL", 1);
    return g_verdict_exit;
  }

  // ---- Step 6: match -> integrated creation proceeds ----------------------
  IDXGIAdapter1* match = nullptr;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) != S_OK || adapter == nullptr)
      break;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        desc.AdapterLuid.LowPart == required_luid.LowPart &&
        desc.AdapterLuid.HighPart == required_luid.HighPart) {
      match = adapter;
      break;
    }
    adapter->Release();
  }
  bool pass = false;
  std::string failure;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  XrSession session = XR_NULL_HANDLE;
  if (match == nullptr) {
    failure = "no local DXGI adapter matches the runtime-required LUID";
  } else {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_9_1;
    const HRESULT hr = D3D11CreateDevice(
        match, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
        &device, &obtained, &context);
    if (FAILED(hr) || device == nullptr || obtained < min_level) {
      failure = "D3D11CreateDevice on runtime adapter failed";
    } else {
      XrGraphicsBindingD3D11KHR binding{};
      binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
      binding.device = device;
      XrSessionCreateInfo info{};
      info.type = XR_TYPE_SESSION_CREATE_INFO;
      info.next = &binding;
      info.systemId = system;
      const XrResult r = xrCreateSession(instance, &info, &session);
      if (r != XR_SUCCESS) {
        failure = std::string("xrCreateSession failed: ") + XrResultName(r);
      } else {
        pass = true;
      }
    }
  }
  if (session != XR_NULL_HANDLE) xrDestroySession(session);
  if (context != nullptr) context->Release();
  if (device != nullptr) device->Release();
  if (match != nullptr) match->Release();
  xrDestroyInstance(instance);
  factory->Release();

  if (pass) {
    std::cout << "PASS: adapter gate matched and integrated session "
                 "created on the Catalyst adapter.\n";
    Verdict("PASS", 0);
  } else {
    std::cout << "FAIL: " << failure << "\n";
    Verdict("FAIL", 1);
  }
  return g_verdict_exit;
}
