#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <string>

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
  kPhysicalJump = 124,
  kPhysicalCrouch = 125,
  kParkourInput = 126,
  kParkourVaultScan = 127,
  kParkourClimbScan = 128,
  kParkourSlideScan = 129,
  kMode = 115,
  kBackend = 116,
  kFrostyPath = 117,
  kFrostyBrowse = 118,
  kFrostyPack = 119,
  kMotionClip = 120,
  kMotionPlayback = 121,
  kMotionClipBrowse = 122,
  kMotionPlaybackBrowse = 123,
};

HWND g_game_path = nullptr;
HWND g_camera = nullptr;
HWND g_input = nullptr;
HWND g_stereo = nullptr;
HWND g_turn = nullptr;
HWND g_units = nullptr;
HWND g_layer = nullptr;
HWND g_space = nullptr;
HWND g_discover = nullptr;
HWND g_body_overlay = nullptr;
HWND g_physical_jump = nullptr;
HWND g_physical_crouch = nullptr;
HWND g_parkour_input = nullptr;
HWND g_parkour_vault_scan = nullptr;
HWND g_parkour_climb_scan = nullptr;
HWND g_parkour_slide_scan = nullptr;
HWND g_mode = nullptr;
HWND g_backend = nullptr;
HWND g_frosty_path = nullptr;
HWND g_frosty_pack = nullptr;
HWND g_motion_clip = nullptr;
HWND g_motion_playback = nullptr;
HWND g_status = nullptr;
HFONT g_font = nullptr;
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

void SelectCombo(HWND control, const std::wstring& value) {
  if (control == nullptr) return;
  const LRESULT index = SendMessageW(
      control, CB_SELECTSTRING, static_cast<WPARAM>(-1),
      reinterpret_cast<LPARAM>(value.c_str()));
  if (index == CB_ERR) SendMessageW(control, CB_SETCURSEL, 0, 0);
}

void SetStatus(const std::wstring& text) {
  SetWindowTextW(g_status, text.c_str());
}

std::wstring Text(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
  GetWindowTextW(control, value.data(), length + 1);
  value.resize(static_cast<std::size_t>(length));
  return value;
}

bool Checked(HWND control) {
  return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

std::wstring Quote(const std::wstring& value) {
  std::wstring out = L"\"";
  for (wchar_t c : value) {
    if (c == L'\"') out += L"\\\"";
    else out += c;
  }
  out += L"\"";
  return out;
}

void SaveSettings() {
  WriteSetting(L"SettingsVersion", L"2");
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
  WriteSetting(L"ParkourClimbScan", Text(g_parkour_climb_scan));
  WriteSetting(L"ParkourSlideScan", Text(g_parkour_slide_scan));
  WriteSetting(L"Mode", Text(g_mode));
  WriteSetting(L"LaunchBackend", Text(g_backend));
  WriteSetting(L"FrostyExecutable", Text(g_frosty_path));
  WriteSetting(L"FrostyPack", Text(g_frosty_pack));
  WriteSetting(L"RecordMotionClip", Text(g_motion_clip));
  WriteSetting(L"PlayMotionClip", Text(g_motion_playback));
}

void LoadSettings() {
  const bool migrate_legacy_defaults =
      ReadSetting(L"SettingsVersion", L"1") != L"2";
  SetWindowTextW(g_game_path, ReadSetting(L"GamePath", L"").c_str());
  SendMessageW(g_camera, BM_SETCHECK,
               ReadSetting(L"Camera", L"1") == L"1" ? BST_CHECKED
                                                     : BST_UNCHECKED,
               0);
  SendMessageW(g_input, BM_SETCHECK,
               ReadSetting(L"Input", L"1") == L"1" ? BST_CHECKED
                                                    : BST_UNCHECKED,
               0);
  SendMessageW(g_stereo, BM_SETCHECK,
               (migrate_legacy_defaults ||
                ReadSetting(L"Stereo", L"1") == L"1") ? BST_CHECKED
                                                     : BST_UNCHECKED,
               0);
  SelectCombo(g_turn, ReadSetting(L"Turn", L"smooth"));
  SetWindowTextW(g_units, ReadSetting(L"Units", L"100").c_str());
  SelectCombo(g_layer, migrate_legacy_defaults
                           ? L"projection"
                           : ReadSetting(L"Layer", L"projection"));
  SelectCombo(g_space, ReadSetting(L"Space", L"view"));
  SendMessageW(g_discover, BM_SETCHECK,
               ReadSetting(L"DiscoverPalettes", L"0") == L"1"
                   ? BST_CHECKED
                   : BST_UNCHECKED,
               0);
  SendMessageW(g_body_overlay, BM_SETCHECK,
                ReadSetting(L"BodyOverlay", L"0") == L"1" ? BST_CHECKED
                                                             : BST_UNCHECKED,
                0);
  SendMessageW(g_physical_jump, BM_SETCHECK,
               ReadSetting(L"PhysicalJump", L"1") == L"1" ? BST_CHECKED
                                                               : BST_UNCHECKED,
               0);
  SendMessageW(g_physical_crouch, BM_SETCHECK,
               ReadSetting(L"PhysicalCrouch", L"1") == L"1" ? BST_CHECKED
                                                                  : BST_UNCHECKED,
               0);
  SendMessageW(g_parkour_input, BM_SETCHECK,
               ReadSetting(L"ParkourInput", L"0") == L"1" ? BST_CHECKED
                                                               : BST_UNCHECKED,
               0);
  SetWindowTextW(g_parkour_vault_scan,
                 ReadSetting(L"ParkourVaultScan", L"57").c_str());
  SetWindowTextW(g_parkour_climb_scan,
                 ReadSetting(L"ParkourClimbScan", L"18").c_str());
  SetWindowTextW(g_parkour_slide_scan,
                 ReadSetting(L"ParkourSlideScan", L"29").c_str());
  SelectCombo(g_mode, ReadSetting(L"Mode", L"camera"));
  SelectCombo(g_backend, ReadSetting(L"LaunchBackend", L"direct"));
  SetWindowTextW(g_frosty_path,
                 ReadSetting(L"FrostyExecutable", L"").c_str());
  SetWindowTextW(g_frosty_pack, ReadSetting(L"FrostyPack", L"").c_str());
  SetWindowTextW(g_motion_clip,
                 ReadSetting(L"RecordMotionClip", L"").c_str());
  SetWindowTextW(g_motion_playback,
                 ReadSetting(L"PlayMotionClip", L"").c_str());
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

bool Validate(std::wstring* error) {
  const std::wstring game = Text(g_game_path);
  if (game.empty() || GetFileAttributesW(game.c_str()) == INVALID_FILE_ATTRIBUTES) {
    *error = L"Select the retail MirrorsEdgeCatalyst.exe first.";
    return false;
  }
  const std::wstring script = g_root + L"\\launch_preview.ps1";
  if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
    *error = L"The launcher package is incomplete: launch_preview.ps1 is missing.";
    return false;
  }
  const std::wstring layer = Text(g_layer);
  if (layer != L"projection" && layer != L"quad") {
    *error = L"Presentation must be projection or quad.";
    return false;
  }
  const std::wstring space = Text(g_space);
  if (space != L"view" && space != L"local") {
    *error = L"Quad space must be view or local.";
    return false;
  }
  const std::wstring mode = Text(g_mode);
  if (mode != L"camera" && mode != L"mono") {
    *error = L"Runtime mode must be camera or mono.";
    return false;
  }
  const std::wstring backend = Text(g_backend);
  if (backend != L"direct" && backend != L"frosty") {
    *error = L"Launch backend must be direct or frosty.";
    return false;
  }
  if (backend == L"frosty") {
    const std::wstring frosty = Text(g_frosty_path);
    if (frosty.empty() || GetFileAttributesW(frosty.c_str()) == INVALID_FILE_ATTRIBUTES) {
      *error = L"Select an existing FrostyModManager.exe for Frosty launch.";
      return false;
    }
    if (Text(g_frosty_pack).empty()) {
      *error = L"Enter the configured Frosty pack name.";
      return false;
    }
  }
  const std::wstring playback = Text(g_motion_playback);
  if (!playback.empty() &&
      GetFileAttributesW(playback.c_str()) == INVALID_FILE_ATTRIBUTES) {
    *error = L"The selected motion playback clip does not exist.";
    return false;
  }
  return true;
}

void DryRun() {
  std::wstring error;
  if (!Validate(&error)) {
    SetStatus(L"DRY RUN FAILED\r\n" + error);
    return;
  }
  SaveSettings();
  SetStatus(L"DRY RUN PASSED\r\n" + Text(g_game_path) +
            L"\r\nHMD-independent configuration and package checks passed.\r\n"
            L"Camera=" + (Checked(g_camera) ? L"on" : L"off") +
            L"  Input=" + (Checked(g_input) ? L"on" : L"off") +
            L"  Stereo=" + (Checked(g_stereo) ? L"on" : L"off") +
            L"  Mode=" + Text(g_mode) + L"  Backend=" + Text(g_backend) +
            L"  Clip playback=" +
            (Text(g_motion_playback).empty() ? L"off" : L"on"));
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

void Launch() {
  std::wstring error;
  if (!Validate(&error)) {
    SetStatus(L"LAUNCH BLOCKED\r\n" + error);
    return;
  }
  SaveSettings();
  std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File " +
                         Quote(g_root + L"\\launch_preview.ps1") +
                         L" -GamePath " + Quote(Text(g_game_path)) +
                         L" -PackageRoot " + Quote(g_root) +
                         L" -Mode " + Quote(Text(g_mode)) +
                         L" -UnitsPerMeter " + Quote(Text(g_units)) +
                         L" -TurnMode " + Quote(Text(g_turn)) +
                         L" -MonoLayer " + Quote(Text(g_layer)) +
                         L" -QuadSpace " + Quote(Text(g_space));
  command += L" -LaunchBackend " + Quote(Text(g_backend));
  if (Text(g_backend) == L"frosty") {
    command += L" -FrostyPath " + Quote(Text(g_frosty_path)) +
               L" -FrostyPack " + Quote(Text(g_frosty_pack));
  }
  if (Checked(g_camera)) command += L" -EnableCamera";
  if (Checked(g_input)) command += L" -EnableInput";
  if (!Checked(g_stereo)) command += L" -DisableStereo";
  if (Checked(g_discover)) command += L" -DiscoverPalettes";
  if (Checked(g_body_overlay)) command += L" -EnableBodyOverlay";
  if (!Checked(g_physical_jump)) command += L" -DisablePhysicalJump";
  if (!Checked(g_physical_crouch)) command += L" -DisablePhysicalCrouchInput";
  if (Checked(g_parkour_input)) command += L" -EnableParkourInput";
  command += L" -ParkourVaultScan " + Quote(Text(g_parkour_vault_scan)) +
             L" -ParkourClimbScan " + Quote(Text(g_parkour_climb_scan)) +
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
  SetStatus(L"LAUNCHING\r\nThe game and MECVR loader are starting.\r\n"
            L"Close the game normally to detach.");
}

HWND Label(HWND parent, const wchar_t* text, int x, int y, int width,
           int height) {
  HWND control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y,
                               width, height, parent, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return control;
}

HWND Button(HWND parent, const wchar_t* text, int id, int x, int y, int width,
            int height, DWORD style = 0) {
  HWND control = CreateWindowW(L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | style, x, y, width,
                               height, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return control;
}

void ApplyFont(HWND control) {
  if (control != nullptr)
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
      Label(window, L"MECVR", 28, 24, 180, 34);
      Label(window, L"Mirror's Edge Catalyst // VR control center", 30, 58,
            430, 24);
      Label(window, L"GAME EXECUTABLE", 30, 104, 160, 22);
      g_game_path = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
                                  30, 128, 430, 30, window,
                                  reinterpret_cast<HMENU>(kGamePath),
                                  GetModuleHandleW(nullptr), nullptr);
      ApplyFont(g_game_path);
      Button(window, L"Browse", kBrowse, 470, 128, 90, 30);
      Label(window, L"RUNTIME", 30, 184, 160, 22);
      g_camera = Button(window, L"6DoF camera", kCamera, 30, 210, 160, 30,
                        BS_AUTOCHECKBOX);
      g_input = Button(window, L"STRIDE motion + input", kInput, 200, 210, 210,
                       30, BS_AUTOCHECKBOX);
      g_stereo = Button(window, L"Temporal stereo (experimental)", kStereo,
                        30, 250, 280, 30, BS_AUTOCHECKBOX);
      Label(window, L"Mode", 340, 214, 42, 22);
      g_mode = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE |
                                                   CBS_DROPDOWNLIST,
                             385, 210, 120, 200, window,
                             reinterpret_cast<HMENU>(kMode),
                             GetModuleHandleW(nullptr), nullptr);
      SendMessageW(g_mode, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"camera"));
      SendMessageW(g_mode, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"mono"));
      ApplyFont(g_mode);
      Label(window, L"Turn", 340, 254, 42, 22);
      g_turn = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                             385, 250, 120, 200, window,
                             reinterpret_cast<HMENU>(kTurn),
                             GetModuleHandleW(nullptr), nullptr);
      SendMessageW(g_turn, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"smooth"));
      SendMessageW(g_turn, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"snap"));
      ApplyFont(g_turn);
      Label(window, L"Units per meter", 30, 300, 120, 22);
      g_units = CreateWindowW(L"EDIT", L"100", WS_CHILD | WS_VISIBLE | WS_BORDER,
                              150, 296, 80, 30, window,
                              reinterpret_cast<HMENU>(kUnits),
                              GetModuleHandleW(nullptr), nullptr);
      ApplyFont(g_units);
      Label(window, L"Presentation", 270, 300, 100, 22);
      g_layer = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                              375, 296, 135, 200, window,
                              reinterpret_cast<HMENU>(kLayer),
                              GetModuleHandleW(nullptr), nullptr);
      SendMessageW(g_layer, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"projection"));
      SendMessageW(g_layer, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"quad"));
      ApplyFont(g_layer);
      Label(window, L"Quad space", 30, 344, 120, 22);
      g_space = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                              150, 340, 120, 200, window,
                              reinterpret_cast<HMENU>(kSpace),
                              GetModuleHandleW(nullptr), nullptr);
      SendMessageW(g_space, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"view"));
      SendMessageW(g_space, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"local"));
      ApplyFont(g_space);
      g_discover = Button(window, L"Developer palette diagnostics (slow)",
                          kDiscover, 300, 340, 250, 30, BS_AUTOCHECKBOX);
      g_body_overlay = Button(window, L"Draw mod-owned IK body overlay",
                              kBodyOverlay, 30, 382, 250, 30, BS_AUTOCHECKBOX);
      g_physical_jump = Button(window, L"Physical jump",
                               kPhysicalJump, 300, 382, 120, 30,
                               BS_AUTOCHECKBOX);
      g_physical_crouch = Button(window, L"Physical crouch input",
                               kPhysicalCrouch, 430, 382, 145, 30,
                               BS_AUTOCHECKBOX);
      g_parkour_input = Button(window, L"Parkour gameplay bridge",
                               kParkourInput, 30, 410, 250, 30,
                               BS_AUTOCHECKBOX);
      Label(window, L"Parkour scan codes", 30, 615, 135, 22);
      g_parkour_vault_scan = CreateWindowW(
          L"EDIT", L"57", WS_CHILD | WS_VISIBLE | WS_BORDER, 170, 611, 65,
          26, window, reinterpret_cast<HMENU>(kParkourVaultScan),
          GetModuleHandleW(nullptr), nullptr);
      g_parkour_climb_scan = CreateWindowW(
          L"EDIT", L"18", WS_CHILD | WS_VISIBLE | WS_BORDER, 300, 611, 65,
          26, window, reinterpret_cast<HMENU>(kParkourClimbScan),
          GetModuleHandleW(nullptr), nullptr);
      g_parkour_slide_scan = CreateWindowW(
          L"EDIT", L"29", WS_CHILD | WS_VISIBLE | WS_BORDER, 430, 611, 65,
          26, window, reinterpret_cast<HMENU>(kParkourSlideScan),
          GetModuleHandleW(nullptr), nullptr);
      ApplyFont(g_parkour_vault_scan);
      ApplyFont(g_parkour_climb_scan);
      ApplyFont(g_parkour_slide_scan);
      Label(window, L"vault", 170, 638, 65, 18);
      Label(window, L"climb", 300, 638, 65, 18);
      Label(window, L"slide", 430, 638, 65, 18);
      Label(window, L"Presentation controls are safe defaults; diagnostics never write game buffers.",
            300, 410, 250, 18);
      Label(window, L"Backend", 30, 430, 90, 22);
      g_backend = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE |
                                                   CBS_DROPDOWNLIST,
                                120, 426, 140, 200, window,
                                reinterpret_cast<HMENU>(kBackend),
                                GetModuleHandleW(nullptr), nullptr);
      SendMessageW(g_backend, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"direct"));
      SendMessageW(g_backend, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"frosty"));
      ApplyFont(g_backend);
      Label(window, L"Frosty executable", 280, 430, 125, 22);
      g_frosty_path = CreateWindowW(L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_BORDER,
                                    405, 426, 135, 30, window,
                                    reinterpret_cast<HMENU>(kFrostyPath),
                                    GetModuleHandleW(nullptr), nullptr);
      ApplyFont(g_frosty_path);
      Button(window, L"Browse", kFrostyBrowse, 470, 462, 70, 28);
      Label(window, L"Frosty pack", 30, 470, 90, 22);
      g_frosty_pack = CreateWindowW(L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_BORDER,
                                    120, 466, 330, 30, window,
                                    reinterpret_cast<HMENU>(kFrostyPack),
                                    GetModuleHandleW(nullptr), nullptr);
      ApplyFont(g_frosty_pack);
      Label(window, L"Frosty is opt-in; MECVR never edits ModData or game files.",
            30, 505, 510, 22);
      Label(window, L"Record solved motion clip (optional path)", 30, 540, 260, 22);
      g_motion_clip = CreateWindowW(L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_BORDER,
                                    295, 536, 200, 30, window,
                                    reinterpret_cast<HMENU>(kMotionClip),
                                    GetModuleHandleW(nullptr), nullptr);
      ApplyFont(g_motion_clip);
      Button(window, L"Browse", kMotionClipBrowse, 500, 536, 70, 30);
      Label(window, L"Play solved motion clip (optional path)", 30, 580, 260, 22);
      g_motion_playback = CreateWindowW(L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | WS_BORDER,
                                        295, 576, 200, 30, window,
                                        reinterpret_cast<HMENU>(kMotionPlayback),
                                        GetModuleHandleW(nullptr), nullptr);
      ApplyFont(g_motion_playback);
      Button(window, L"Browse", kMotionPlaybackBrowse, 500, 576, 70, 30);
      Label(window, L"Playback loops; live head/camera tracking remains active.",
            30, 665, 510, 22);
      Button(window, L"LAUNCH VR", kLaunch, 30, 700, 170, 42, BS_DEFPUSHBUTTON);
      Button(window, L"Dry run", kDryRun, 210, 700, 120, 42);
      Button(window, L"Save", kSave, 340, 700, 100, 42);
      Label(window, L"STATUS", 30, 770, 160, 22);
      g_status = CreateWindowW(L"EDIT", L"Ready. HMD is not required for Dry run.",
                               WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                   ES_READONLY | WS_VSCROLL,
                               30, 796, 530, 130, window,
                               reinterpret_cast<HMENU>(kStatus),
                               GetModuleHandleW(nullptr), nullptr);
      ApplyFont(g_status);
      LoadSettings();
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kBrowse: BrowseGame(window); return 0;
        case kFrostyBrowse: BrowseFrosty(window); return 0;
        case kMotionClipBrowse: BrowseMotionClip(window, true, g_motion_clip); return 0;
        case kMotionPlaybackBrowse:
          BrowseMotionClip(window, false, g_motion_playback);
          return 0;
        case kLaunch: Launch(); return 0;
        case kDryRun: DryRun(); return 0;
        case kSave: SaveSettings(); SetStatus(L"Settings saved."); return 0;
      }
      break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, RGB(226, 232, 240));
      SetBkColor(dc, RGB(18, 24, 38));
      static HBRUSH brush = CreateSolidBrush(RGB(18, 24, 38));
      return reinterpret_cast<LRESULT>(brush);
    }
    case WM_DESTROY:
      if (g_font != nullptr) DeleteObject(g_font);
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
        L"launch_preview.ps1",
        L"bin\\mecvr_inject.exe",
        L"bin\\mecvr_runtime.dll",
        L"bin\\mecvr_m3b_live.dll",
        L"bin\\mecvr_m2b_live.dll",
        L"bin\\openxr_loader.dll",
        L"bin\\mecvr_launcher.exe",
    };
    for (const wchar_t* relative : required) {
      if (GetFileAttributesW((g_root + L"\\" + relative).c_str()) ==
          INVALID_FILE_ATTRIBUTES) {
        return 1;
      }
    }
    return 0;
  }
  WNDCLASSW klass{};
  klass.hInstance = instance;
  klass.lpfnWndProc = WindowProc;
  klass.lpszClassName = L"MECVRLauncherWindow";
  klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  klass.hbrBackground = CreateSolidBrush(RGB(18, 24, 38));
  RegisterClassW(&klass);
  HWND window = CreateWindowExW(0, klass.lpszClassName,
                               L"MECVR // Catalyst VR", WS_OVERLAPPED |
                                   WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, 610, 940, nullptr,
                               nullptr, instance, nullptr);
  ShowWindow(window, show);
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
