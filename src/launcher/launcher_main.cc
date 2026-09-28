#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <cwchar>
#include <iterator>
#include <string>

#include "mecvr_version.h"

namespace {

enum : int {
  kGamePath = 100,
  kBrowse = 101,
  kCamera = 102,
  kInput = 103,
  kStereo = 104,
  kTurn = 105,
  kUnits = 106,
  kLaunch = 107,
  kDryRun = 108,
  kSave = 109,
  kStatus = 110,
  kLayer = 111,
  kSpace = 112,
  kDiscover = 113,
  kBodyOverlay = 114,
  kMode = 115,
  kBackend = 116,
  kFrostyPath = 117,
  kFrostyBrowse = 118,
  kFrostyPack = 119,
  kMotionClip = 120,
  kMotionPlayback = 121,
  kMotionClipBrowse = 122,
  kMotionPlaybackBrowse = 123,
  kPhysicalJump = 124,
  kPhysicalCrouch = 125,
  kParkourInput = 126,
  kParkourVaultScan = 127,
  kMagRopeScan = 128,
  kParkourSlideScan = 129,
  kPerformance = 130,
  kRuntimePacing = 131,
  kNativeBoneMap = 132,
  kCaptureNativeBoneMap = 133,
  kNativeBoneMapCapturePath = 134,
  kAdvanced = 140,
  kHmdPreset = 141,
  kReadyGame = 150,
  kReadyPackage = 151,
  kReadyLoader = 152,
  kReadyImmersive = 153,
};

constexpr COLORREF kBackground = RGB(12, 16, 22);
constexpr COLORREF kSurface = RGB(20, 27, 36);
constexpr COLORREF kSurfaceRaised = RGB(27, 36, 47);
constexpr COLORREF kControl = RGB(16, 23, 31);
constexpr COLORREF kBorder = RGB(54, 67, 82);
constexpr COLORREF kText = RGB(238, 242, 245);
constexpr COLORREF kMuted = RGB(157, 170, 181);
constexpr COLORREF kRed = RGB(224, 48, 64);
constexpr COLORREF kGreen = RGB(75, 205, 139);
constexpr COLORREF kAmber = RGB(244, 177, 77);

HWND g_game_path = nullptr;
HWND g_camera = nullptr;
HWND g_input = nullptr;
HWND g_stereo = nullptr;
HWND g_turn = nullptr;
HWND g_units = nullptr;
HWND g_layer = nullptr;
HWND g_space = nullptr;
HWND g_mode = nullptr;
HWND g_performance = nullptr;
HWND g_backend = nullptr;
HWND g_frosty_path = nullptr;
HWND g_frosty_pack = nullptr;
HWND g_motion_clip = nullptr;
HWND g_motion_playback = nullptr;
HWND g_discover = nullptr;
HWND g_body_overlay = nullptr;
HWND g_physical_jump = nullptr;
HWND g_physical_crouch = nullptr;
HWND g_parkour_input = nullptr;
HWND g_parkour_vault_scan = nullptr;
HWND g_mag_rope_scan = nullptr;
HWND g_parkour_slide_scan = nullptr;
HWND g_runtime_pacing = nullptr;
HWND g_native_bone_map = nullptr;
HWND g_capture_native_bone_map = nullptr;
HWND g_native_bone_map_capture_path = nullptr;
HWND g_status = nullptr;
HWND g_hmd_summary = nullptr;
HWND g_ready_game = nullptr;
HWND g_ready_package = nullptr;
HWND g_ready_loader = nullptr;
HWND g_ready_immersive = nullptr;
HWND g_advanced_window = nullptr;

HFONT g_font = nullptr;
HFONT g_small_font = nullptr;
HFONT g_heading_font = nullptr;
HFONT g_title_font = nullptr;
HBRUSH g_background_brush = nullptr;
HBRUSH g_surface_brush = nullptr;
HBRUSH g_control_brush = nullptr;
std::wstring g_root;

std::wstring ModuleRoot() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return L".";
  std::wstring path(buffer, length);
  const std::size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring ConfigPath() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                               MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return g_root + L"\\mecvr.ini";
  const std::wstring dir = std::wstring(buffer, length) + L"\\MECVR";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\launcher.ini";
}

std::wstring ReadSetting(const wchar_t* key, const wchar_t* fallback) {
  wchar_t value[2048] = {};
  GetPrivateProfileStringW(L"launcher", key, fallback, value,
                           static_cast<DWORD>(std::size(value)),
                           ConfigPath().c_str());
  return value;
}

void WriteSetting(const wchar_t* key, const std::wstring& value) {
  WritePrivateProfileStringW(L"launcher", key, value.c_str(),
                             ConfigPath().c_str());
}

bool Exists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring VersionText() {
  return L"v" + std::to_wstring(MECVR_VERSION_MAJOR) + L"." +
         std::to_wstring(MECVR_VERSION_UPDATE) + L"." +
         std::to_wstring(MECVR_VERSION_PATCH) + L"." +
         std::to_wstring(MECVR_VERSION_MINOR_CHANGE) + L"-" +
         MECVR_VERSION_SUFFIX_W;
}

void SelectCombo(HWND control, const std::wstring& value) {
  if (control == nullptr) return;
  const LRESULT index = SendMessageW(
      control, CB_SELECTSTRING, static_cast<WPARAM>(-1),
      reinterpret_cast<LPARAM>(value.c_str()));
  if (index == CB_ERR) SendMessageW(control, CB_SETCURSEL, 0, 0);
}

void SetStatus(const std::wstring& text) {
  if (g_status != nullptr) SetWindowTextW(g_status, text.c_str());
}

std::wstring Text(HWND control) {
  if (control == nullptr) return {};
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
  GetWindowTextW(control, value.data(), length + 1);
  value.resize(static_cast<std::size_t>(length));
  return value;
}

bool Checked(HWND control) {
  return control != nullptr &&
         SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SetChecked(HWND control, bool checked) {
  if (control != nullptr)
    SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED,
                 0);
}

std::wstring Quote(const std::wstring& value) {
  std::wstring out = L"\"";
  for (wchar_t c : value) {
    if (c == L'"') out += L"\\\"";
    else out += c;
  }
  out += L"\"";
  return out;
}

void SaveSettings() {
  WriteSetting(L"SettingsVersion", L"4");
  WriteSetting(L"GamePath", Text(g_game_path));
  WriteSetting(L"Camera", Checked(g_camera) ? L"1" : L"0");
  WriteSetting(L"Input", Checked(g_input) ? L"1" : L"0");
  WriteSetting(L"Stereo", Checked(g_stereo) ? L"1" : L"0");
  WriteSetting(L"Turn", Text(g_turn));
  WriteSetting(L"Units", Text(g_units));
  WriteSetting(L"Layer", Text(g_layer));
  WriteSetting(L"Space", Text(g_space));
  WriteSetting(L"DiscoverPalettes", Checked(g_discover) ? L"1" : L"0");
  WriteSetting(L"BodyOverlay", Checked(g_body_overlay) ? L"1" : L"0");
  WriteSetting(L"PhysicalJump", Checked(g_physical_jump) ? L"1" : L"0");
  WriteSetting(L"PhysicalCrouch", Checked(g_physical_crouch) ? L"1" : L"0");
  WriteSetting(L"ParkourInput", Checked(g_parkour_input) ? L"1" : L"0");
  WriteSetting(L"ParkourVaultScan", Text(g_parkour_vault_scan));
  WriteSetting(L"MagRopeScan", Text(g_mag_rope_scan));
  WriteSetting(L"ParkourSlideScan", Text(g_parkour_slide_scan));
  WriteSetting(L"Mode", Text(g_mode));
  WriteSetting(L"LaunchBackend", Text(g_backend));
  WriteSetting(L"FrostyExecutable", Text(g_frosty_path));
  WriteSetting(L"FrostyPack", Text(g_frosty_pack));
  WriteSetting(L"RecordMotionClip", Text(g_motion_clip));
  WriteSetting(L"PlayMotionClip", Text(g_motion_playback));
  WriteSetting(L"PerformanceMode", Text(g_performance));
  WriteSetting(L"PreserveRuntimePacing", Checked(g_runtime_pacing) ? L"1" : L"0");
  WriteSetting(L"NativeBoneMap", Text(g_native_bone_map));
  WriteSetting(L"CaptureNativeBoneMap",
               Checked(g_capture_native_bone_map) ? L"1" : L"0");
  WriteSetting(L"NativeBoneMapCapturePath",
               Text(g_native_bone_map_capture_path));
}

void LoadSettings() {
  const bool legacy = ReadSetting(L"SettingsVersion", L"1") != L"4";
  SetWindowTextW(g_game_path, ReadSetting(L"GamePath", L"").c_str());
  SetChecked(g_camera, ReadSetting(L"Camera", L"1") == L"1");
  SetChecked(g_input, ReadSetting(L"Input", L"1") == L"1");
  SetChecked(g_stereo, legacy || ReadSetting(L"Stereo", L"1") == L"1");
  SelectCombo(g_turn, ReadSetting(L"Turn", L"smooth"));
  SetWindowTextW(g_units, ReadSetting(L"Units", L"100").c_str());
  SelectCombo(g_layer, legacy ? L"projection"
                             : ReadSetting(L"Layer", L"projection"));
  SelectCombo(g_space, ReadSetting(L"Space", L"view"));
  SetChecked(g_discover, ReadSetting(L"DiscoverPalettes", L"0") == L"1");
  SetChecked(g_body_overlay, ReadSetting(L"BodyOverlay", L"0") == L"1");
  SetChecked(g_physical_jump, ReadSetting(L"PhysicalJump", L"1") == L"1");
  SetChecked(g_physical_crouch,
             ReadSetting(L"PhysicalCrouch", L"1") == L"1");
  SetChecked(g_parkour_input, ReadSetting(L"ParkourInput", L"0") == L"1");
  SetWindowTextW(g_parkour_vault_scan,
                 ReadSetting(L"ParkourVaultScan", L"57").c_str());
  SetWindowTextW(g_mag_rope_scan,
                 ReadSetting(L"MagRopeScan", L"16").c_str());
  SetWindowTextW(g_parkour_slide_scan,
                 ReadSetting(L"ParkourSlideScan", L"29").c_str());
  SelectCombo(g_mode, ReadSetting(L"Mode", L"camera"));
  SelectCombo(g_backend, ReadSetting(L"LaunchBackend", L"direct"));
  SetWindowTextW(g_frosty_path,
                 ReadSetting(L"FrostyExecutable", L"").c_str());
  SetWindowTextW(g_frosty_pack,
                 ReadSetting(L"FrostyPack", L"").c_str());
  SetWindowTextW(g_motion_clip,
                 ReadSetting(L"RecordMotionClip", L"").c_str());
  SetWindowTextW(g_motion_playback,
                 ReadSetting(L"PlayMotionClip", L"").c_str());
  SelectCombo(g_performance,
              ReadSetting(L"PerformanceMode", L"performance"));
  SetChecked(g_runtime_pacing,
             ReadSetting(L"PreserveRuntimePacing", L"1") == L"1");
  SetWindowTextW(g_native_bone_map,
                 ReadSetting(L"NativeBoneMap", L"").c_str());
  SetChecked(g_capture_native_bone_map,
             ReadSetting(L"CaptureNativeBoneMap", L"0") == L"1");
  SetWindowTextW(g_native_bone_map_capture_path,
                 ReadSetting(L"NativeBoneMapCapturePath", L"").c_str());
}

void BrowseGame(HWND owner) {
  wchar_t path[MAX_PATH] = {};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"Mirror's Edge Catalyst\0MirrorsEdgeCatalyst.exe\0"
                       L"Executables\0*.exe\0\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (GetOpenFileNameW(&dialog)) SetWindowTextW(g_game_path, path);
}

void BrowseFrosty(HWND owner) {
  wchar_t path[MAX_PATH] = {};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"Frosty Mod Manager\0FrostyModManager.exe\0"
                       L"Executables\0*.exe\0\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = MAX_PATH;
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (GetOpenFileNameW(&dialog)) SetWindowTextW(g_frosty_path, path);
}

void BrowseMotionClip(HWND owner, bool save, HWND target) {
  wchar_t path[MAX_PATH] = {};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"MECVR motion clip\0*.mecvrclip\0All files\0*.*\0\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrDefExt = L"mecvrclip";
  dialog.Flags = OFN_PATHMUSTEXIST |
                 (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  const BOOL selected = save ? GetSaveFileNameW(&dialog)
                             : GetOpenFileNameW(&dialog);
  if (selected != FALSE && target != nullptr) SetWindowTextW(target, path);
}

bool Validate(std::wstring* error) {
  const std::wstring game = Text(g_game_path);
  if (!Exists(game)) {
    *error = L"Select the retail MirrorsEdgeCatalyst.exe first.";
    return false;
  }
  if (!Exists(g_root + L"\\launch_preview.ps1")) {
    *error = L"The launcher package is incomplete: launch_preview.ps1 is missing.";
    return false;
  }
  const std::wstring mode = Text(g_mode);
  const std::wstring module = mode == L"mono"
                                  ? L"\\bin\\mecvr_m2b_live.dll"
                                  : L"\\bin\\mecvr_m3b_live.dll";
  if (!Exists(g_root + L"\\bin\\mecvr_inject.exe") ||
      !Exists(g_root + L"\\bin\\openxr_loader.dll") ||
      !Exists(g_root + module)) {
    *error = L"The selected package is missing a required runtime binary.";
    return false;
  }
  if (Text(g_layer) != L"projection" && Text(g_layer) != L"quad") {
    *error = L"Presentation must be projection or quad.";
    return false;
  }
  if (Text(g_space) != L"view" && Text(g_space) != L"local") {
    *error = L"Quad space must be view or local.";
    return false;
  }
  if (mode != L"camera" && mode != L"mono") {
    *error = L"Runtime mode must be camera or mono.";
    return false;
  }
  const std::wstring backend = Text(g_backend);
  if (backend != L"direct" && backend != L"frosty") {
    *error = L"Launch backend must be direct or frosty.";
    return false;
  }
  if (backend == L"frosty" &&
      (!Exists(Text(g_frosty_path)) || Text(g_frosty_pack).empty())) {
    *error = L"Frosty launch requires an existing manager and pack name.";
    return false;
  }
  if (!Text(g_motion_playback).empty() &&
      !Exists(Text(g_motion_playback))) {
    *error = L"The selected motion playback clip does not exist.";
    return false;
  }
  if (!Text(g_native_bone_map).empty() &&
      !Exists(Text(g_native_bone_map))) {
    *error = L"The selected native Faith bone contract does not exist.";
    return false;
  }
  return true;
}

bool HmdReady() {
  return Exists(Text(g_game_path)) &&
         Exists(g_root + L"\\launch_preview.ps1") &&
         Exists(g_root + L"\\bin\\openxr_loader.dll") &&
         Exists(g_root + L"\\bin\\mecvr_m3b_live.dll") &&
         Checked(g_camera) && Checked(g_input) && Checked(g_stereo) &&
         Text(g_mode) == L"camera" && Text(g_layer) == L"projection";
}

void SetReadinessLine(HWND control, const wchar_t* label, bool ok) {
  if (control == nullptr) return;
  SetWindowTextW(control, (std::wstring(ok ? L"●  " : L"○  ") + label).c_str());
  InvalidateRect(control, nullptr, TRUE);
}

void RefreshReadiness() {
  const bool game = Exists(Text(g_game_path));
  const bool package = Exists(g_root + L"\\launch_preview.ps1") &&
                       Exists(g_root + L"\\bin\\mecvr_inject.exe");
  const bool loader = Exists(g_root + L"\\bin\\openxr_loader.dll") &&
                      Exists(g_root + L"\\bin\\mecvr_m3b_live.dll");
  const bool immersive = Checked(g_camera) && Checked(g_input) &&
                         Checked(g_stereo) && Text(g_mode) == L"camera" &&
                         Text(g_layer) == L"projection";
  SetReadinessLine(g_ready_game, L"Catalyst executable", game);
  SetReadinessLine(g_ready_package, L"MECVR package", package);
  SetReadinessLine(g_ready_loader, L"OpenXR / M3B runtime", loader);
  SetReadinessLine(g_ready_immersive, L"Immersive Quest 3 profile", immersive);
  const bool ready = game && package && loader && immersive;
  SetWindowTextW(
      g_hmd_summary,
      ready ? L"READY TO LAUNCH  /  HMD connection checked at startup"
            : L"SETUP REQUIRED  /  resolve the items below before HMD testing");
  InvalidateRect(g_hmd_summary, nullptr, TRUE);
}

void ApplyHmdPreset() {
  SetChecked(g_camera, true);
  SetChecked(g_input, true);
  SetChecked(g_stereo, true);
  SetChecked(g_body_overlay, false);
  SelectCombo(g_mode, L"camera");
  SelectCombo(g_layer, L"projection");
  SelectCombo(g_performance, L"performance");
  SetStatus(L"Quest 3 / VDXR HMD profile applied.\r\n"
            L"Camera, stereo, motion input, projection, and performance are ready.");
  RefreshReadiness();
}

void DryRun() {
  std::wstring error;
  if (!Validate(&error)) {
    SetStatus(L"DRY RUN FAILED\r\n" + error);
    RefreshReadiness();
    return;
  }
  SaveSettings();
  SetStatus(L"DRY RUN PASSED\r\n" + Text(g_game_path) +
            L"\r\nPackage and launch validation passed.\r\n" +
            (HmdReady() ? L"Quest 3 / VDXR profile is ready."
                         : L"Valid launch configuration; HMD profile is not active."));
  RefreshReadiness();
}

void Launch() {
  std::wstring error;
  if (!Validate(&error)) {
    SetStatus(L"LAUNCH BLOCKED\r\n" + error);
    RefreshReadiness();
    return;
  }
  SaveSettings();
  std::wstring command =
      L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File " +
      Quote(g_root + L"\\launch_preview.ps1") + L" -GamePath " +
      Quote(Text(g_game_path)) + L" -PackageRoot " + Quote(g_root) +
      L" -Mode " + Quote(Text(g_mode)) + L" -UnitsPerMeter " +
      Quote(Text(g_units)) + L" -TurnMode " + Quote(Text(g_turn)) +
      L" -MonoLayer " + Quote(Text(g_layer)) + L" -QuadSpace " +
      Quote(Text(g_space)) + L" -PerformanceMode " +
      Quote(Text(g_performance));
  if (Checked(g_runtime_pacing)) command += L" -PreserveRuntimePacing";
  if (!Text(g_native_bone_map).empty())
    command += L" -NativeBoneMap " + Quote(Text(g_native_bone_map));
  if (Checked(g_capture_native_bone_map)) command += L" -CaptureNativeBoneMap";
  if (!Text(g_native_bone_map_capture_path).empty())
    command += L" -NativeBoneMapCapturePath " +
               Quote(Text(g_native_bone_map_capture_path));
  command += L" -LaunchBackend " + Quote(Text(g_backend));
  if (Text(g_backend) == L"frosty")
    command += L" -FrostyPath " + Quote(Text(g_frosty_path)) +
               L" -FrostyPack " + Quote(Text(g_frosty_pack));
  if (Checked(g_camera)) command += L" -EnableCamera";
  if (Checked(g_input)) command += L" -EnableInput";
  if (Checked(g_stereo)) command += L" -EnableStereo";
  else command += L" -DisableStereo";
  if (Checked(g_discover)) command += L" -DiscoverPalettes";
  if (Checked(g_body_overlay)) command += L" -EnableBodyOverlay";
  else command += L" -DisableBodyOverlay";
  if (!Checked(g_physical_jump)) command += L" -DisablePhysicalJump";
  if (!Checked(g_physical_crouch)) command += L" -DisablePhysicalCrouchInput";
  if (Checked(g_parkour_input)) command += L" -EnableParkourInput";
  command += L" -ParkourVaultScan " + Quote(Text(g_parkour_vault_scan)) +
             L" -MagRopeScan " + Quote(Text(g_mag_rope_scan)) +
             L" -ParkourSlideScan " + Quote(Text(g_parkour_slide_scan));
  if (!Text(g_motion_clip).empty())
    command += L" -RecordMotionClip " + Quote(Text(g_motion_clip));
  if (!Text(g_motion_playback).empty())
    command += L" -PlayMotionClip " + Quote(Text(g_motion_playback));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::wstring mutable_command = command;
  if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, g_root.c_str(), &startup,
                      &process)) {
    SetStatus(L"LAUNCH FAILED\r\n" + std::to_wstring(GetLastError()));
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  SetStatus(L"LAUNCHING\r\nCatalystVR is attaching the OpenXR bridge.\r\n"
            L"Put on the Quest 3 and confirm VDXR is active.");
}

HWND Label(HWND parent, const wchar_t* text, int x, int y, int width,
           int height, int id = 0, HFONT font = nullptr) {
  HWND control = CreateWindowExW(
      0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, width,
      height, parent, id == 0 ? nullptr
                              : reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT,
               reinterpret_cast<WPARAM>(font != nullptr ? font : g_font), TRUE);
  return control;
}

HWND Edit(HWND parent, const wchar_t* value, int id, int x, int y, int width,
          int height) {
  HWND control = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", value,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, width,
      height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return control;
}

HWND Button(HWND parent, const wchar_t* text, int id, int x, int y, int width,
            int height, DWORD style = 0, bool accent = false) {
  const DWORD button_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | style |
                             (accent ? BS_OWNERDRAW : 0);
  HWND control = CreateWindowW(
      L"BUTTON", text, button_style, x, y, width, height, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return control;
}

HWND Combo(HWND parent, int id, int x, int y, int width, int height) {
  HWND control = CreateWindowW(
      L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
      x, y, width, height, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return control;
}

void AddComboValue(HWND combo, const wchar_t* value) {
  SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
}

void DrawCard(HDC dc, int left, int top, int right, int bottom) {
  HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
  HBRUSH brush = CreateSolidBrush(kSurface);
  HGDIOBJ old_pen = SelectObject(dc, pen);
  HGDIOBJ old_brush = SelectObject(dc, brush);
  RoundRect(dc, left, top, right, bottom, 12, 12);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(brush);
  DeleteObject(pen);
}

void PaintMain(HWND window, HDC dc) {
  RECT client{};
  GetClientRect(window, &client);
  HBRUSH background = CreateSolidBrush(kBackground);
  FillRect(dc, &client, background);
  DeleteObject(background);
  RECT header{0, 0, client.right, 96};
  HBRUSH header_brush = CreateSolidBrush(RGB(15, 21, 29));
  FillRect(dc, &header, header_brush);
  DeleteObject(header_brush);
  HPEN red_pen = CreatePen(PS_SOLID, 5, kRed);
  HGDIOBJ old_pen = SelectObject(dc, red_pen);
  MoveToEx(dc, 38, 30, nullptr);
  LineTo(dc, 38, 72);
  SelectObject(dc, old_pen);
  DeleteObject(red_pen);
  DrawCard(dc, 32, 114, 574, 244);
  DrawCard(dc, 606, 114, 1148, 332);
  DrawCard(dc, 32, 264, 574, 494);
  DrawCard(dc, 606, 350, 1148, 758);
  DrawCard(dc, 32, 510, 574, 758);
  DrawCard(dc, 32, 778, 1148, 850);
}

void ApplyHmdControlLayout(HWND parent) {
  g_camera = Button(parent, L"6DoF camera", kCamera, 56, 312, 180, 30,
                    BS_AUTOCHECKBOX);
  g_input = Button(parent, L"Motion input", kInput, 248, 312, 160, 30,
                   BS_AUTOCHECKBOX);
  g_stereo = Button(parent, L"Stereo projection", kStereo, 420, 312, 140, 30,
                    BS_AUTOCHECKBOX);
}

LRESULT CALLBACK AdvancedProc(HWND window, UINT message, WPARAM wparam,
                              LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      Label(window, L"ADVANCED RUNTIME SETTINGS", 28, 22, 500, 28,
            0, g_heading_font);
      Label(window, L"These controls are optional for normal Quest 3 testing.",
            28, 52, 600, 20, 0, g_small_font);
      g_discover = Button(window, L"Palette diagnostics (slow)", kDiscover,
                          28, 92, 210, 28, BS_AUTOCHECKBOX);
      g_body_overlay = Button(window, L"Mod-owned IK overlay", kBodyOverlay,
                              252, 92, 180, 28, BS_AUTOCHECKBOX);
      g_physical_jump = Button(window, L"Physical jump", kPhysicalJump,
                               446, 92, 140, 28, BS_AUTOCHECKBOX);
      g_physical_crouch = Button(window, L"Physical crouch input",
                                 kPhysicalCrouch, 28, 126, 210, 28,
                                 BS_AUTOCHECKBOX);
      g_parkour_input = Button(window, L"Parkour gameplay bridge",
                               kParkourInput, 252, 126, 210, 28,
                               BS_AUTOCHECKBOX);
      g_runtime_pacing = Button(window, L"Preserve runtime pacing / AFR",
                                kRuntimePacing, 28, 160, 250, 28,
                                BS_AUTOCHECKBOX);
      Label(window, L"BACKEND", 28, 208, 200, 20, 0, g_heading_font);
      Label(window, L"Launch route", 28, 240, 100, 22);
      g_backend = Combo(window, kBackend, 132, 236, 150, 180);
      AddComboValue(g_backend, L"direct");
      AddComboValue(g_backend, L"frosty");
      Label(window, L"Frosty manager", 306, 240, 110, 22);
      g_frosty_path = Edit(window, L"", kFrostyPath, 418, 236, 250, 28);
      Button(window, L"Browse", kFrostyBrowse, 674, 236, 80, 28);
      Label(window, L"Frosty pack", 28, 276, 100, 22);
      g_frosty_pack = Edit(window, L"", kFrostyPack, 132, 272, 500, 28);
      Label(window, L"INPUT ROUTES", 28, 324, 200, 20, 0, g_heading_font);
      Label(window, L"Vault", 28, 356, 55, 22);
      g_parkour_vault_scan = Edit(window, L"57", kParkourVaultScan, 82, 352,
                                  55, 28);
      Label(window, L"MAG rope", 154, 356, 75, 22);
      g_mag_rope_scan = Edit(window, L"16", kMagRopeScan, 230, 352, 55, 28);
      Label(window, L"Slide", 302, 356, 55, 22);
      g_parkour_slide_scan = Edit(window, L"29", kParkourSlideScan, 360, 352,
                                  55, 28);
      Label(window, L"scan codes", 428, 356, 100, 22, 0, g_small_font);
      Label(window, L"MOTION CLIPS", 28, 404, 200, 20, 0, g_heading_font);
      Label(window, L"Record", 28, 438, 70, 22);
      g_motion_clip = Edit(window, L"", kMotionClip, 102, 434, 530, 28);
      Button(window, L"Browse", kMotionClipBrowse, 642, 434, 80, 28);
      Label(window, L"Playback", 28, 474, 70, 22);
      g_motion_playback = Edit(window, L"", kMotionPlayback, 102, 470, 530,
                               28);
      Button(window, L"Browse", kMotionPlaybackBrowse, 642, 470, 80, 28);
      Label(window, L"NATIVE FAITH CONTRACT", 28, 520, 250, 20, 0,
            g_heading_font);
      g_native_bone_map = Edit(window, L"", kNativeBoneMap, 28, 552, 560, 28);
      g_capture_native_bone_map = Button(
          window, L"Capture one-shot contract", kCaptureNativeBoneMap, 600, 552,
          155, 28, BS_AUTOCHECKBOX);
      Label(window, L"Output path (optional)", 28, 590, 150, 22, 0,
            g_small_font);
      g_native_bone_map_capture_path =
          Edit(window, L"", kNativeBoneMapCapturePath, 180, 586, 575, 28);
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kFrostyBrowse: BrowseFrosty(window); return 0;
        case kMotionClipBrowse:
          BrowseMotionClip(window, true, g_motion_clip);
          return 0;
        case kMotionPlaybackBrowse:
          BrowseMotionClip(window, false, g_motion_playback);
          return 0;
      }
      if (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == EN_CHANGE)
        RefreshReadiness();
      break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, kText);
      SetBkColor(dc, message == WM_CTLCOLORSTATIC ? kSurface : kControl);
      return reinterpret_cast<LRESULT>(message == WM_CTLCOLORSTATIC
                                           ? g_surface_brush
                                           : g_control_brush);
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      FillRect(dc, &client, g_surface_brush);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_CLOSE:
      ShowWindow(window, SW_HIDE);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wparam,
                          LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      Label(window, L"CATALYSTVR", 58, 24, 400, 42, 0, g_title_font);
      Label(window, L"Mirror's Edge Catalyst  /  immersive control center", 60,
            68, 600, 20, 0, g_small_font);
      Label(window, VersionText().c_str(), 1000, 34, 120, 24, 0,
            g_small_font);
      Label(window, L"GAME TARGET", 56, 132, 220, 22, 0, g_heading_font);
      g_game_path = Edit(window, L"", kGamePath, 56, 166, 420, 30);
      Button(window, L"BROWSE", kBrowse, 488, 166, 88, 30);
      Label(window, L"Select the retail executable. MECVR never edits the game directory.",
            56, 207, 500, 20, 0, g_small_font);

      Label(window, L"HMD READINESS", 630, 132, 300, 22, 0, g_heading_font);
      g_hmd_summary = Label(window, L"Checking local package…", 630, 166, 470,
                            24, 0, g_font);
      g_ready_game = Label(window, L"○  Catalyst executable", 630, 202, 470, 22,
                           kReadyGame, g_small_font);
      g_ready_package = Label(window, L"○  MECVR package", 630, 228, 470, 22,
                              kReadyPackage, g_small_font);
      g_ready_loader = Label(window, L"○  OpenXR / M3B runtime", 630, 254, 470,
                             22, kReadyLoader, g_small_font);
      g_ready_immersive = Label(window, L"○  Immersive Quest 3 profile", 630,
                                280, 470, 22, kReadyImmersive, g_small_font);
      Button(window, L"APPLY QUEST 3 / VDXR PROFILE", kHmdPreset, 630, 304,
             260, 30, 0, true);

      Label(window, L"RUNTIME PROFILE", 56, 270, 280, 22, 0, g_heading_font);
      g_camera = Button(window, L"6DoF camera", kCamera, 56, 306, 170, 30,
                        BS_AUTOCHECKBOX);
      g_input = Button(window, L"Motion input", kInput, 240, 306, 160, 30,
                       BS_AUTOCHECKBOX);
      g_stereo = Button(window, L"Stereo projection", kStereo, 414, 306, 150,
                        30, BS_AUTOCHECKBOX);
      Label(window, L"Mode", 56, 356, 55, 22);
      g_mode = Combo(window, kMode, 112, 352, 130, 180);
      AddComboValue(g_mode, L"camera");
      AddComboValue(g_mode, L"mono");
      Label(window, L"Performance", 264, 356, 90, 22);
      g_performance = Combo(window, kPerformance, 358, 352, 130, 180);
      AddComboValue(g_performance, L"performance");
      AddComboValue(g_performance, L"balanced");
      AddComboValue(g_performance, L"diagnostic");
      Label(window, L"Turn", 56, 402, 55, 22);
      g_turn = Combo(window, kTurn, 112, 398, 130, 180);
      AddComboValue(g_turn, L"smooth");
      AddComboValue(g_turn, L"snap");
      Label(window, L"Units / meter", 264, 402, 90, 22);
      g_units = Edit(window, L"100", kUnits, 358, 398, 130, 30);
      Label(window, L"Presentation", 56, 448, 90, 22);
      g_layer = Combo(window, kLayer, 150, 444, 130, 180);
      AddComboValue(g_layer, L"projection");
      AddComboValue(g_layer, L"quad");
      Label(window, L"Quad space", 304, 448, 80, 22);
      g_space = Combo(window, kSpace, 390, 444, 130, 180);
      AddComboValue(g_space, L"view");
      AddComboValue(g_space, L"local");
      Label(window, L"Projection is the immersive path; quad is a diagnostic theatre fallback.",
            56, 478, 500, 20, 0, g_small_font);

      Label(window, L"LAUNCH ROUTE", 56, 532, 240, 22, 0, g_heading_font);
      Label(window, L"Backend", 56, 570, 70, 22);
      g_backend = Combo(window, kBackend, 132, 566, 130, 180);
      AddComboValue(g_backend, L"direct");
      AddComboValue(g_backend, L"frosty");
      Label(window, L"HMD testing uses Direct by default and injects before renderer startup.",
            56, 612, 500, 20, 0, g_small_font);
      Label(window, L"Status", 56, 650, 70, 22);
      g_status = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT",
          L"Ready. Apply the Quest 3 / VDXR profile before your first test.",
          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
          56, 678, 500, 56, window, reinterpret_cast<HMENU>(kStatus),
          GetModuleHandleW(nullptr), nullptr);
      SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_small_font),
                   TRUE);

      Label(window, L"TESTING NOTES", 630, 370, 280, 22, 0, g_heading_font);
      Label(window, L"Quest 3 / VDXR", 630, 408, 200, 24, 0, g_font);
      Label(window, L"The runtime owns headset resolution and refresh.\r\n"
                     L"MECVR requests square-eye composition and preserves\r\n"
                     L"performance mode for AFR/frame generation compatibility.",
            630, 438, 470, 64, 0, g_small_font);
      Label(window, L"Motion: grip = fist  /  grip + swing = combat\r\n"
                     L"Pull grip toward yourself = MAG rope  /  jump-held = ledge climb",
            630, 520, 470, 44, 0, g_small_font);
      Button(window, L"ADVANCED SETTINGS", kAdvanced, 630, 600, 220, 34, 0,
             true);
      Label(window, L"Optional diagnostics, scan codes, Frosty, motion clips, and Faith contract tools.",
            630, 644, 470, 36, 0, g_small_font);

      Label(window, L"A public alpha build for headset testing. Use Dry run first if the game is not ready.",
            56, 798, 900, 20, 0, g_small_font);
      Button(window, L"LAUNCH VR", kLaunch, 630, 790, 230, 46, 0, true);
      Button(window, L"DRY RUN", kDryRun, 872, 790, 116, 46, 0, false);
      Button(window, L"SAVE", kSave, 1000, 790, 100, 46, 0, false);
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kBrowse: BrowseGame(window); RefreshReadiness(); return 0;
        case kHmdPreset: ApplyHmdPreset(); return 0;
        case kAdvanced:
          if (g_advanced_window != nullptr) {
            ShowWindow(g_advanced_window, SW_SHOWNORMAL);
            SetForegroundWindow(g_advanced_window);
          }
          return 0;
        case kLaunch: Launch(); return 0;
        case kDryRun: DryRun(); return 0;
        case kSave: SaveSettings(); RefreshReadiness(); SetStatus(L"Settings saved."); return 0;
      }
      if (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == BN_CLICKED ||
          HIWORD(wparam) == EN_CHANGE)
        RefreshReadiness();
      break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, kText);
      return reinterpret_cast<LRESULT>(message == WM_CTLCOLOREDIT
                                           ? g_control_brush
                                           : g_background_brush);
    }
    case WM_DRAWITEM: {
      auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (draw == nullptr) break;
      const bool primary = draw->CtlID == kLaunch || draw->CtlID == kHmdPreset;
      const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
      HBRUSH brush = CreateSolidBrush(primary ? (pressed ? RGB(176, 35, 51) : kRed)
                                             : kSurfaceRaised);
      FillRect(draw->hDC, &draw->rcItem, brush);
      DeleteObject(brush);
      HPEN pen = CreatePen(PS_SOLID, 1, primary ? kRed : kBorder);
      HGDIOBJ old = SelectObject(draw->hDC, pen);
      SelectObject(draw->hDC, GetStockObject(NULL_BRUSH));
      Rectangle(draw->hDC, draw->rcItem.left, draw->rcItem.top,
                draw->rcItem.right, draw->rcItem.bottom);
      SelectObject(draw->hDC, old);
      DeleteObject(pen);
      wchar_t text[128] = {};
      GetWindowTextW(draw->hwndItem, text, 128);
      SetBkMode(draw->hDC, TRANSPARENT);
      SetTextColor(draw->hDC, kText);
      DrawTextW(draw->hDC, text, -1, &draw->rcItem,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return TRUE;
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      PaintMain(window, dc);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DESTROY:
      if (g_advanced_window != nullptr) DestroyWindow(g_advanced_window);
      if (g_font != nullptr) DeleteObject(g_font);
      if (g_small_font != nullptr) DeleteObject(g_small_font);
      if (g_heading_font != nullptr) DeleteObject(g_heading_font);
      if (g_title_font != nullptr) DeleteObject(g_title_font);
      if (g_background_brush != nullptr) DeleteObject(g_background_brush);
      if (g_surface_brush != nullptr) DeleteObject(g_surface_brush);
      if (g_control_brush != nullptr) DeleteObject(g_control_brush);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  g_root = ModuleRoot();
  const wchar_t* command_line = GetCommandLineW();
  if (command_line != nullptr && wcsstr(command_line, L"--self-test") != nullptr) {
    const wchar_t* required[] = {
        L"launch_preview.ps1", L"bin\\mecvr_inject.exe",
        L"bin\\mecvr_runtime.dll", L"bin\\mecvr_m3b_live.dll",
        L"bin\\mecvr_m2b_live.dll", L"bin\\openxr_loader.dll",
        L"bin\\mecvr_launcher.exe",
    };
    for (const wchar_t* relative : required) {
      if (!Exists(g_root + L"\\" + relative)) return 1;
    }
    return 0;
  }

  g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  g_small_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  g_heading_font = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  g_title_font = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  g_background_brush = CreateSolidBrush(kBackground);
  g_surface_brush = CreateSolidBrush(kSurface);
  g_control_brush = CreateSolidBrush(kControl);

  WNDCLASSW main_class{};
  main_class.hInstance = instance;
  main_class.lpfnWndProc = MainProc;
  main_class.lpszClassName = L"MECVRLauncherWindow";
  main_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  main_class.hbrBackground = g_background_brush;
  RegisterClassW(&main_class);
  WNDCLASSW advanced_class = main_class;
  advanced_class.lpfnWndProc = AdvancedProc;
  advanced_class.lpszClassName = L"MECVRAdvancedWindow";
  RegisterClassW(&advanced_class);

  HWND window = CreateWindowExW(
      0, main_class.lpszClassName, L"CatalystVR // Control Center",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 1180, 920, nullptr, nullptr, instance,
      nullptr);
  g_advanced_window = CreateWindowExW(
      WS_EX_TOOLWINDOW, advanced_class.lpszClassName,
      L"CatalystVR // Advanced settings",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 800, 690, window, nullptr, instance,
      nullptr);
  ShowWindow(window, show);
  UpdateWindow(window);
  LoadSettings();
  RefreshReadiness();
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
