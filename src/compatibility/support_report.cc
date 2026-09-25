// M0A static/process baseline builder: externally observable facts only.
#include "compatibility/support_report.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <psapi.h>
#include <sstream>
#include <tlhelp32.h>

namespace mecvr::compat {
namespace {

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string BasenameLower(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  const std::string base =
      slash == std::string::npos ? path : path.substr(slash + 1);
  return ToLower(base);
}

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
  return out;
}

// --- Minimal public-domain-style SHA-256 (FIPS 180-4), streaming. ---
class Sha256 {
 public:
  Sha256() { Reset(); }
  void Reset() {
    h_ = {0x6a09e667ul, 0xbb67ae85ul, 0x3c6ef372ul, 0xa54ff53aul,
          0x510e527ful, 0x9b05688cul, 0x1f83d9abul, 0x5be0cd19ul};
    total_ = 0;
    buf_len_ = 0;
  }
  void Update(const unsigned char* data, size_t len) {
    total_ += len;
    while (len > 0) {
      const size_t take =
          (std::min)(sizeof(buf_) - buf_len_, len);
      for (size_t i = 0; i < take; ++i) buf_[buf_len_ + i] = data[i];
      buf_len_ += take;
      data += take;
      len -= take;
      if (buf_len_ == sizeof(buf_)) {
        Compress(buf_);
        buf_len_ = 0;
      }
    }
  }
  std::array<unsigned char, 32> Final() {
    const std::uint64_t bit_len = total_ * 8u;
    unsigned char pad = 0x80;
    Update(&pad, 1);
    unsigned char zero = 0;
    while (buf_len_ != 56) Update(&zero, 1);
    unsigned char len_bytes[8] = {};
    std::uint64_t v = bit_len;
    for (int i = 7; i >= 0; --i) {
      len_bytes[i] = static_cast<unsigned char>(v & 0xffu);
      v >>= 8;
    }
    Update(len_bytes, 8);
    std::array<unsigned char, 32> out = {};
    for (int i = 0; i < 8; ++i) {
      out[static_cast<size_t>(i) * 4 + 0] =
          static_cast<unsigned char>((h_[i] >> 24) & 0xffu);
      out[static_cast<size_t>(i) * 4 + 1] =
          static_cast<unsigned char>((h_[i] >> 16) & 0xffu);
      out[static_cast<size_t>(i) * 4 + 2] =
          static_cast<unsigned char>((h_[i] >> 8) & 0xffu);
      out[static_cast<size_t>(i) * 4 + 3] =
          static_cast<unsigned char>(h_[i] & 0xffu);
    }
    return out;
  }

 private:
  static std::uint32_t RoR(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
  }
  void Compress(const unsigned char block[64]) {
    static const std::uint32_t k[64] = {
        0x428a2f98ul, 0x71374491ul, 0xb5c0fbcful, 0xe9b5dba5ul,
        0x3956c25bul, 0x59f111f1ul, 0x923f82a4ul, 0xab1c5ed5ul,
        0xd807aa98ul, 0x12835b01ul, 0x243185beul, 0x550c7dc3ul,
        0x72be5d74ul, 0x80deb1feul, 0x9bdc06a7ul, 0xc19bf174ul,
        0xe49b69c1ul, 0xefbe4786ul, 0x0fc19dc6ul, 0x240ca1ccul,
        0x2de92c6ful, 0x4a7484aaul, 0x5cb0a9dcul, 0x76f988daul,
        0x983e5152ul, 0xa831c66dul, 0xb00327c8ul, 0xbf597fc7ul,
        0xc6e00bf3ul, 0xd5a79147ul, 0x06ca6351ul, 0x14292967ul,
        0x27b70a85ul, 0x2e1b2138ul, 0x4d2c6dfcul, 0x53380d13ul,
        0x650a7354ul, 0x766a0abbul, 0x81c2c92eul, 0x92722c85ul,
        0xa2bfe8a1ul, 0xa81a664bul, 0xc24b8b70ul, 0xc76c51a3ul,
        0xd192e819ul, 0xd6990624ul, 0xf40e3585ul, 0x106aa070ul,
        0x19a4c116ul, 0x1e376c08ul, 0x2748774cul, 0x34b0bcb5ul,
        0x391c0cb3ul, 0x4ed8aa4aul, 0x5b9cca4ful, 0x682e6ff3ul,
        0x748f82eeul, 0x78a5636ful, 0x84c87814ul, 0x8cc70208ul,
        0x90befffaul, 0xa4506cebul, 0xbef9a3f7ul, 0xc67178f2ul};
    std::uint32_t w[64] = {};
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
             (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 =
          RoR(w[i - 15], 7) ^ RoR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 =
          RoR(w[i - 2], 17) ^ RoR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = RoR(e, 6) ^ RoR(e, 11) ^ RoR(e, 25);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t t1 = hh + s1 + ch + k[i] + w[i];
      const std::uint32_t s0 = RoR(a, 2) ^ RoR(a, 13) ^ RoR(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += hh;
  }
  std::array<std::uint32_t, 8> h_;
  std::uint64_t total_ = 0;
  unsigned char buf_[64] = {};
  size_t buf_len_ = 0;
};

std::string Sha256File(const std::string& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open file for hashing";
    return {};
  }
  Sha256 sha;
  char chunk[65536];
  while (in) {
    in.read(chunk, sizeof(chunk));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      sha.Update(reinterpret_cast<const unsigned char*>(chunk),
                 static_cast<size_t>(got));
    }
  }
  if (in.bad()) {
    error = "read error during hashing";
    return {};
  }
  char hex[65] = {};
  const auto digest = sha.Final();
  for (size_t i = 0; i < digest.size(); ++i) {
    std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
  }
  return std::string(hex, 64);
}

std::wstring ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n =
      ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<size_t>(n) - 1, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  return out;
}

void QueryVersions(const std::string& path, std::string& file_ver,
                   std::string& product_ver) {
  const std::wstring w = ToWide(path);
  const DWORD size = ::GetFileVersionInfoSizeW(w.c_str(), nullptr);
  if (size == 0) return;
  std::vector<unsigned char> buf(size);
  if (!::GetFileVersionInfoW(w.c_str(), 0, size, buf.data())) return;
  VS_FIXEDFILEINFO* fixed = nullptr;
  UINT fixed_len = 0;
  if (::VerQueryValueW(buf.data(), L"\\",
                       reinterpret_cast<void**>(&fixed), &fixed_len) &&
      fixed != nullptr && fixed_len >= sizeof(*fixed)) {
    char tmp[64] = {};
    std::snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u",
                  static_cast<unsigned>(HIWORD(fixed->dwFileVersionMS)),
                  static_cast<unsigned>(LOWORD(fixed->dwFileVersionMS)),
                  static_cast<unsigned>(HIWORD(fixed->dwFileVersionLS)),
                  static_cast<unsigned>(LOWORD(fixed->dwFileVersionLS)));
    file_ver = tmp;
    std::snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u",
                  static_cast<unsigned>(HIWORD(fixed->dwProductVersionMS)),
                  static_cast<unsigned>(LOWORD(fixed->dwProductVersionMS)),
                  static_cast<unsigned>(HIWORD(fixed->dwProductVersionLS)),
                  static_cast<unsigned>(LOWORD(fixed->dwProductVersionLS)));
    product_ver = tmp;
  }
}

struct WindowPick {
  HWND hwnd = nullptr;
  std::string title;
  std::string cls;
  int client_w = 0;
  int client_h = 0;
};

BOOL CALLBACK EnumWindowsCb(HWND hwnd, LPARAM lparam) {
  auto* ctx = reinterpret_cast<std::pair<DWORD, WindowPick>*>(lparam);
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->first) return TRUE;
  if (!::IsWindowVisible(hwnd)) return TRUE;
  RECT rc = {};
  if (!::GetClientRect(hwnd, &rc)) return TRUE;
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return TRUE;  // skip invisible/zero-area windows.
  if (w * h <= ctx->second.client_w * ctx->second.client_h) return TRUE;
  wchar_t title[512] = {};
  wchar_t cls[256] = {};
  ::GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
  ::GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
  ctx->second.hwnd = hwnd;
  ctx->second.title = WideToUtf8(title);
  ctx->second.cls = WideToUtf8(cls);
  ctx->second.client_w = w;
  ctx->second.client_h = h;
  return TRUE;
}

bool FindMainWindow(DWORD pid, WindowPick& out) {
  std::pair<DWORD, WindowPick> ctx{pid, {}};
  ::EnumWindows(EnumWindowsCb, reinterpret_cast<LPARAM>(&ctx));
  if (ctx.second.hwnd == nullptr) return false;
  out = ctx.second;
  return true;
}

std::string DescribeWindowMode(HWND hwnd, int client_w, int client_h) {
  const LONG_PTR style =
      ::GetWindowLongPtrW(hwnd, GWL_STYLE);
  const LONG_PTR exstyle =
      ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  char buf[512] = {};
  std::snprintf(buf, sizeof(buf),
                "style=0x%08lx exstyle=0x%08lx popup=%d overlappedwindow=%d",
                static_cast<unsigned long>(style),
                static_cast<unsigned long>(exstyle),
                (style & WS_POPUP) != 0 ? 1 : 0,
                (style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW ? 1 : 0);
  std::string evidence = buf;
  RECT win_rc = {};
  ::GetWindowRect(hwnd, &win_rc);
  const int win_w = win_rc.right - win_rc.left;
  const int win_h = win_rc.bottom - win_rc.top;
  HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  std::string mode = "unknown";
  if (::GetMonitorInfoW(mon, &mi)) {
    const int mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
    const int mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    char buf2[256] = {};
    std::snprintf(buf2, sizeof(buf2),
                  " client=%dx%d window=%dx%d monitor=%dx%d",
                  client_w, client_h, win_w, win_h, mon_w, mon_h);
    evidence += buf2;
    const bool borderless_popup =
        ((style & WS_POPUP) != 0) &&
        ((style & WS_OVERLAPPEDWINDOW) == 0);
    if (borderless_popup && client_w >= mon_w && client_h >= mon_h) {
      mode = "fullscreen-borderless";
    } else {
      mode = "windowed";
    }
  }
  evidence += " => " + mode;
  return evidence;
}

void CollectModules(DWORD pid, const std::string& game_dir_lower,
                    std::vector<ModuleEntry>& modules,
                    std::vector<OverlayFinding>& findings) {
  HANDLE proc = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                              FALSE, pid);
  if (proc == nullptr) return;
  HMODULE mods[2048] = {};
  DWORD needed = 0;
  if (::EnumProcessModulesEx(proc, mods, sizeof(mods), &needed,
                             LIST_MODULES_ALL)) {
    const size_t count =
        static_cast<size_t>(needed) / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i) {
      wchar_t path[32768] = {};
      if (::GetModuleFileNameExW(proc, mods[i], path,
                                 static_cast<DWORD>(std::size(path))) == 0) {
        continue;
      }
      ModuleEntry e;
      e.path = WideToUtf8(path);
      e.basename_lower = BasenameLower(e.path);
      e.from_game_dir =
          !game_dir_lower.empty() &&
          ToLower(e.path).rfind(game_dir_lower, 0) == 0;
      modules.push_back(e);
    }
  }
  ::CloseHandle(proc);

  // Best-effort known-basename list (T7 owns deeper inspection).
  auto flag = [&](const std::string& base, const std::string& category) {
    for (const auto& m : modules) {
      if (m.basename_lower == base) {
        findings.push_back({base, m.path, category});
      }
    }
  };
  flag("reshade64.dll", "reshade");
  flag("reshade32.dll", "reshade");
  flag("rtsshooks64.dll", "rtss");
  flag("rtsshooks.dll", "rtss");
  flag("specialk64.dll", "specialk");
  flag("specialk32.dll", "specialk");
  flag("gameoverlayrenderer64.dll", "overlay-steam");
  flag("gameoverlayrenderer.dll", "overlay-steam");
  flag("discordhook64.dll", "overlay-discord");
  flag("discordhook.dll", "overlay-discord");
  // Proxy suspects: DXGI/D3D11/dinput shims loaded from the game dir instead
  // of System32/WinSxS (a plain basename hit outside the game dir is normal).
  for (const auto& m : modules) {
    if (!m.from_game_dir) continue;
    if (m.basename_lower == "dxgi.dll" ||
        m.basename_lower == "d3d11.dll" ||
        m.basename_lower == "dinput8.dll") {
      findings.push_back(
          {m.basename_lower, m.path, "proxy-suspect(game-dir)"});
    }
  }
}

bool ParentOf(DWORD pid, DWORD& ppid, std::string& parent_name) {
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  DWORD found_parent = 0;
  std::wstring found_parent_image;
  // First pass: locate self to get parent pid; second pass: parent image.
  if (::Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ProcessID == pid) {
        found_parent = pe.th32ParentProcessID;
        break;
      }
    } while (::Process32NextW(snap, &pe));
  }
  if (found_parent != 0 && ::Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ProcessID == found_parent) {
        found_parent_image = pe.szExeFile;
        break;
      }
    } while (::Process32NextW(snap, &pe));
  }
  ::CloseHandle(snap);
  if (found_parent == 0) return false;
  ppid = found_parent;
  parent_name = WideToUtf8(found_parent_image);
  return true;
}

}  // namespace

std::vector<ExeFingerprint> FingerprintExes(const std::string& game_dir) {
  static const char* kNames[] = {"MirrorsEdgeCatalyst.exe",
                                 "MirrorsEdgeCatalystTrial.exe"};
  std::vector<ExeFingerprint> out;
  for (const char* name : kNames) {
    ExeFingerprint fp;
    fp.file_name = name;
    fp.path = (std::filesystem::path(game_dir) / name).string();
    std::error_code ec;
    fp.exists = std::filesystem::exists(fp.path, ec) && !ec;
    if (!fp.exists) {
      fp.error = "file not present on disk";
      out.push_back(fp);
      continue;
    }
    fp.file_size =
        static_cast<std::uint64_t>(std::filesystem::file_size(fp.path, ec));
    if (ec) fp.error = "cannot stat file size";
    fp.sha256 = Sha256File(fp.path, fp.error);
    QueryVersions(fp.path, fp.file_version, fp.product_version);
    out.push_back(fp);
  }
  return out;
}

LiveProcessBaseline CollectLiveBaseline(const std::string& retail_exe_path,
                                        int wait_seconds) {
  LiveProcessBaseline base;
  base.attempted = true;
  const std::wstring w_exe = ToWide(retail_exe_path);
  const std::wstring w_dir = ToWide(
      std::filesystem::path(retail_exe_path).parent_path().string());

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  std::wstring cmdline = L"\"" + w_exe + L"\"";
  if (!::CreateProcessW(w_exe.c_str(), cmdline.data(), nullptr, nullptr,
                        FALSE, 0, nullptr,
                        w_dir.empty() ? nullptr : w_dir.c_str(), &si, &pi)) {
    char buf[128] = {};
    std::snprintf(buf, sizeof(buf), "CreateProcess failed gle=%lu",
                  static_cast<unsigned long>(::GetLastError()));
    base.error = buf;
    return base;
  }
  base.pid = pi.dwProcessId;
  base.launched_by_tool = true;
  ::CloseHandle(pi.hThread);

  WindowPick pick;
  const int deadline_ticks = wait_seconds > 0 ? wait_seconds : 120;
  for (int waited = 0; waited < deadline_ticks; ++waited) {
    DWORD exit_code = STILL_ACTIVE;
    if (::GetExitCodeProcess(pi.hProcess, &exit_code) &&
        exit_code != STILL_ACTIVE) {
      char buf[128] = {};
      std::snprintf(buf, sizeof(buf),
                    "game exited before a window appeared (code=%lu)",
                    static_cast<unsigned long>(exit_code));
      base.error = buf;
      break;
    }
    if (FindMainWindow(pi.dwProcessId, pick)) {
      base.found_window = true;
      break;
    }
    ::Sleep(1000);
  }
  if (!base.found_window && base.error.empty()) {
    base.error = "no visible main window within wait budget";
  }

  if (base.found_window) {
    base.hwnd = reinterpret_cast<std::uint64_t>(pick.hwnd);
    base.window_title = pick.title;
    base.window_class = pick.cls;
    base.client_w = pick.client_w;
    base.client_h = pick.client_h;
    base.window_evidence =
        DescribeWindowMode(pick.hwnd, pick.client_w, pick.client_h);
    base.window_mode =
        base.window_evidence.find("fullscreen-borderless") != std::string::npos
            ? "fullscreen-borderless"
            : (base.window_evidence.find("windowed") != std::string::npos
                   ? "windowed"
                   : "unknown");
    std::string game_dir_lower =
        ToLower(std::filesystem::path(retail_exe_path).parent_path().string());
    CollectModules(pi.dwProcessId, game_dir_lower, base.modules,
                   base.overlay_findings);
    DWORD ppid = 0;
    std::string parent;
    if (ParentOf(pi.dwProcessId, ppid, parent)) {
      base.parent_pid = ppid;
      base.parent_name = parent;
    }
    const std::string parent_lower = ToLower(base.parent_name);
    if (parent_lower.find("frostymodmanager") != std::string::npos) {
      base.launcher_note = "parent is FrostyModManager (modded launch path)";
    } else if (!base.parent_name.empty()) {
      base.launcher_note =
          "parent is " + base.parent_name + " (not FrostyModManager)";
    } else {
      base.launcher_note = "parent unknown (tool-launched)";
    }
  }

  // Clean shutdown: polite close first, terminate only if it refuses.
  if (base.found_window && pick.hwnd != nullptr) {
    ::PostMessageW(pick.hwnd, WM_CLOSE, 0, 0);
    const DWORD wait = ::WaitForSingleObject(pi.hProcess, 15000);
    if (wait == WAIT_OBJECT_0) {
      base.terminated_cleanly = true;
    } else {
      ::TerminateProcess(pi.hProcess, 0);
      base.terminated_cleanly =
          ::WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0;
      if (!base.terminated_cleanly && base.error.empty()) {
        base.error = "game did not exit after close + terminate";
      }
    }
  } else {
    ::TerminateProcess(pi.hProcess, 0);
    base.terminated_cleanly =
        ::WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0;
  }
  ::CloseHandle(pi.hProcess);
  return base;
}

SupportReport BuildStaticBaseline(const std::string& game_dir, bool do_launch,
                                  int wait_seconds) {
  SupportReport report;
  report.game_dir = game_dir;
  report.exes = FingerprintExes(game_dir);
  if (do_launch && !report.exes.empty() && report.exes[0].exists) {
    report.live = CollectLiveBaseline(report.exes[0].path, wait_seconds);
  } else if (do_launch) {
    report.live.attempted = true;
    report.live.error = "retail exe missing on disk; launch skipped";
  }
  return report;
}

std::string SupportReport::ToText() const {
  std::ostringstream os;
  os << "MECVR M0A static/process baseline\n";
  os << "game_dir=" << game_dir << "\n";
  for (const auto& e : exes) {
    os << "[exe] name=" << e.file_name << " exists=" << (e.exists ? 1 : 0)
       << " size=" << e.file_size << " sha256="
       << (e.sha256.empty() ? "-" : e.sha256)
       << " file_version=" << (e.file_version.empty() ? "-" : e.file_version)
       << " product_version="
       << (e.product_version.empty() ? "-" : e.product_version);
    if (!e.error.empty()) os << " error=" << e.error;
    os << "\n";
  }
  if (exes.size() == 2 && exes[0].exists && exes[1].exists) {
    os << "retail-vs-trial: "
       << (exes[0].sha256 != exes[1].sha256 ? "distinct builds" : "IDENTICAL?")
       << " (retail " << exes[0].file_size << " bytes vs trial "
       << exes[1].file_size << " bytes)\n";
  }
  const auto& l = live;
  os << "[live] attempted=" << (l.attempted ? 1 : 0)
     << " found_window=" << (l.found_window ? 1 : 0) << " pid=" << l.pid
     << " ppid=" << l.parent_pid << " parent=" 
     << (l.parent_name.empty() ? "-" : l.parent_name) << "\n";
  if (l.found_window) {
    os << "[window] hwnd=0x" << std::hex << l.hwnd << std::dec << " title=\""
       << l.window_title << "\" class=\"" << l.window_class << "\" client="
       << l.client_w << "x" << l.client_h << " mode=" << l.window_mode
       << "\n";
    os << "  evidence: " << l.window_evidence << "\n";
  }
  os << "[modules] count=" << l.modules.size() << "\n";
  for (const auto& m : l.modules) {
    os << "  " << (m.from_game_dir ? "[game-dir] " : "[system]  ") << m.path
       << "\n";
  }
  os << "[overlays] findings=" << l.overlay_findings.size() << "\n";
  for (const auto& f : l.overlay_findings) {
    os << "  " << f.category << ": " << f.path << "\n";
  }
  if (l.overlay_findings.empty() && l.found_window) {
    os << "  none of the known basenames matched (best-effort only)\n";
  }
  os << "[launcher] " << (l.launcher_note.empty() ? "-" : l.launcher_note)
     << "\n";
  if (!l.error.empty()) os << "[live-error] " << l.error << "\n";
  if (l.attempted && l.found_window) {
    os << "[shutdown] terminated_cleanly=" << (l.terminated_cleanly ? 1 : 0)
       << "\n";
  }
  os << "scope-note: static/process facts only; no D3D11 claims (T3B owns).\n";
  return os.str();
}

}  // namespace mecvr::compat
