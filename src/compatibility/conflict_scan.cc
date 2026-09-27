// T7 compatibility scanner: BEST-EFFORT conflict detection (never exhaustive).
//
// Read-only inspection only: EnumProcessModulesEx + GetModuleFileNameExW +
// bounded ReadProcessMemory of a few DXGI export prologue bytes. This file
// never writes to another process (no WriteProcessMemory, no remote threads,
// no hooks, no injection). Finding absence proves nothing about unknown
// Present hooks; see docs/COMPATIBILITY.md.
#include "compatibility/conflict_scan.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <psapi.h>
#include <sstream>
#include <tlhelp32.h>

namespace mecvr::compat::scan {
namespace {

std::string ToLower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string BasenameLower(const std::string& path) {
  const std::size_t slash = path.find_last_of("\\/");
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
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
  return out;
}

std::string HexBytes(const unsigned char* bytes, std::size_t len) {
  std::string out;
  char tmp[4] = {};
  for (std::size_t i = 0; i < len; ++i) {
    std::snprintf(tmp, sizeof(tmp), "%02x", bytes[i]);
    out += tmp;
    if (i + 1 < len) out += ' ';
  }
  return out;
}

// Module base + size snapshot for detour-target ownership attribution.
struct ModuleRange {
  std::string path;
  std::string basename_lower;
  std::uint64_t base = 0;
  std::uint64_t size = 0;
};

}  // namespace

bool LooksLikeHookedPrologue(const unsigned char* bytes, std::size_t len,
                             std::string& reason) {
  if (bytes == nullptr || len == 0) {
    reason = "unreadable prologue";
    return false;
  }
  // JMP rel32 (E9): classic detour thunk.
  if (len >= 5 && bytes[0] == 0xE9) {
    reason = "E9 JMP rel32 thunk";
    return true;
  }
  // Short JMP rel8 (EB) as the very first instruction is thunk-shaped.
  // (EB later in a function body is normal control flow; only pos 0 counts.)
  if (len >= 2 && bytes[0] == 0xEB) {
    reason = "EB short-JMP at entry";
    return true;
  }
  // Indirect JMP FF 25 (import-thunk style detour overwrite) at entry.
  if (len >= 6 && bytes[0] == 0xFF && bytes[1] == 0x25) {
    reason = "FF 25 indirect-JMP at entry";
    return true;
  }
  // PUSH imm32 + RET (68 xx xx xx xx C3) is a known detour stub shape.
  if (len >= 6 && bytes[0] == 0x68 && bytes[5] == 0xC3) {
    reason = "PUSH+RET stub shape";
    return true;
  }
  reason.clear();
  return false;
}

std::vector<ModuleEntry> EnumerateProcessModules(std::uint32_t pid,
                                                 const std::string& game_dir,
                                                 std::string& error) {
  std::vector<ModuleEntry> out;
  const std::string game_dir_lower = ToLower(game_dir);
  DWORD open_err = 0;
  HANDLE proc = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                              FALSE, static_cast<DWORD>(pid));
  if (proc == nullptr) {
    // BEST-EFFORT fallback (documented): DRM/anti-tamper handle protection
    // can deny OpenProcess to non-parent scanners (observed: gle=5 on the
    // live retail game while OS tooling such as tasklist /m still lists its
    // modules). Fall back to a read-only Toolhelp module snapshot, which
    // grants no write capability and touches nothing in the target.
    open_err = ::GetLastError();
    HANDLE snap = ::CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, static_cast<DWORD>(pid));
    if (snap == INVALID_HANDLE_VALUE) {
      char buf[128] = {};
      std::snprintf(buf, sizeof(buf),
                    "OpenProcess(%u) failed gle=%lu; snapshot gle=%lu",
                    static_cast<unsigned>(pid),
                    static_cast<unsigned long>(open_err),
                    static_cast<unsigned long>(::GetLastError()));
      error = buf;
      return out;
    }
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    if (::Module32FirstW(snap, &me)) {
      do {
        ModuleEntry e;
        e.path = WideToUtf8(me.szExePath);
        e.basename_lower = BasenameLower(e.path);
        e.from_game_dir = !game_dir_lower.empty() &&
                          ToLower(e.path).rfind(game_dir_lower, 0) == 0;
        out.push_back(e);
      } while (::Module32NextW(snap, &me));
    }
    ::CloseHandle(snap);
    if (out.empty()) {
      error = "Toolhelp snapshot yielded no modules";
      return out;
    }
    // Non-fatal note (not a hard error): caller still gets the module list;
    // remote prologue reads stay unavailable and are skipped downstream.
    char buf[256] = {};
    std::snprintf(buf, sizeof(buf),
                  "note: OpenProcess denied gle=%lu (suspected DRM handle "
                  "protection); module list via read-only Toolhelp snapshot; "
                  "remote prologue reads unavailable",
                  static_cast<unsigned long>(open_err));
    error = buf;
    return out;
  }
  HMODULE mods[4096] = {};
  DWORD needed = 0;
  if (!::EnumProcessModulesEx(proc, mods, sizeof(mods), &needed,
                              LIST_MODULES_ALL)) {
    char buf[128] = {};
    std::snprintf(buf, sizeof(buf), "EnumProcessModulesEx failed gle=%lu",
                  static_cast<unsigned long>(::GetLastError()));
    error = buf;
    ::CloseHandle(proc);
    return out;
  }
  const std::size_t count = static_cast<std::size_t>(needed) / sizeof(HMODULE);
  for (std::size_t i = 0; i < count; ++i) {
    wchar_t path[32768] = {};
    if (::GetModuleFileNameExW(proc, mods[i], path,
                               static_cast<DWORD>(std::size(path))) == 0) {
      continue;
    }
    ModuleEntry e;
    e.path = WideToUtf8(path);
    e.basename_lower = BasenameLower(e.path);
    e.from_game_dir = !game_dir_lower.empty() &&
                      ToLower(e.path).rfind(game_dir_lower, 0) == 0;
    out.push_back(e);
  }
  ::CloseHandle(proc);
  return out;
}

std::vector<OverlayFinding> MatchKnownModules(
    const std::vector<ModuleEntry>& modules) {
  // Best-effort known-basename list. Matching proves nothing beyond
  // "this basename is loaded"; non-matching proves nothing at all.
  static const struct {
    const char* base;
    const char* category;
  } kKnown[] = {
      // ReShade proxies / add-ons.
      {"reshade64.dll", "reshade"},
      {"reshade32.dll", "reshade"},
      {"dxgi.dll", "reshade-or-proxy-candidate"},
      {"d3d11.dll", "reshade-or-proxy-candidate"},
      // SpecialK.
      {"specialk64.dll", "specialk"},
      {"specialk32.dll", "specialk"},
      {"specialk.dll", "specialk"},
      // RTSS / MSI Afterburner hooks.
      {"rtsshooks64.dll", "rtss"},
      {"rtsshooks.dll", "rtss"},
      // Steam overlay.
      {"gameoverlayrenderer64.dll", "overlay-steam"},
      {"gameoverlayrenderer.dll", "overlay-steam"},
      // Discord overlay.
      {"discordhook64.dll", "overlay-discord"},
      {"discordhook.dll", "overlay-discord"},
      // Catalyst asset-launcher/plugin shims. These can change load order or
      // data-path behavior around Frosty-managed assets.
      {"datapathfixplugin.dll", "frosty-datapath"},
      {"frostyplugin.dll", "frosty-plugin"},
      // Generic shim names that are only suspicious from the game dir.
  };
  std::vector<OverlayFinding> out;
  for (const auto& k : kKnown) {
    const bool dir_sensitive =
        std::string(k.base) == "dxgi.dll" || std::string(k.base) == "d3d11.dll";
    for (const auto& m : modules) {
      if (m.basename_lower != k.base) continue;
      if (dir_sensitive && !m.from_game_dir) continue;  // system copy normal.
      out.push_back({k.base, m.path, k.category});
    }
  }
  // Game-dir shim suspects beyond dxgi/d3d11 (e.g. dinput8 proxy DLLs).
  for (const auto& m : modules) {
    if (!m.from_game_dir) continue;
    if (m.basename_lower == "dinput8.dll" || m.basename_lower == "dsound.dll" ||
        m.basename_lower == "version.dll" || m.basename_lower == "winmm.dll") {
      out.push_back({m.basename_lower, m.path, "proxy-suspect(game-dir)"});
    }
  }
  return out;
}

namespace {

// Resolve remote export address safely: remote_base + (local_export -
// local_base). Only valid for the same DLL image; caller verifies basename.
bool RemoteExportAddress(HANDLE proc, HMODULE remote_base, FARPROC local_export,
                         HMODULE local_base, std::uint64_t& remote_addr) {
  if (remote_base == nullptr || local_export == nullptr ||
      local_base == nullptr) {
    return false;
  }
  const std::uint64_t rva =
      reinterpret_cast<std::uint64_t>(local_export) -
      reinterpret_cast<std::uint64_t>(local_base);
  remote_addr = reinterpret_cast<std::uint64_t>(remote_base) + rva;
  (void)proc;
  return true;
}

std::string OwnerOf(const std::vector<ModuleRange>& ranges,
                    std::uint64_t addr) {
  for (const auto& r : ranges) {
    if (addr >= r.base && addr < r.base + r.size) return r.path;
  }
  return {};
}

}  // namespace

ConflictScanResult ScanPid(std::uint32_t pid, const std::string& game_dir) {
  ConflictScanResult r;
  r.pid = pid;

  // 1. Module enumeration (read-only snapshot).
  r.modules = EnumerateProcessModules(pid, game_dir, r.error);
  if (!r.error.empty() && r.modules.empty()) return r;

  // 2. Known-basename match (best-effort, never exhaustive).
  r.known_hits = MatchKnownModules(r.modules);

  // 3. DXGI export-prologue inspection where safe.
  // Present/ResizeBuffers are vtable methods, not exports, so no remote
  // vtable resolution is attempted (that would need an in-process device).
  // Instead: bounded read-only prologue checks of DXGI factory exports as a
  // proxy for "is dxgi.dll entry code thunk-shaped (detour-like)?".
  static const char* kExports[] = {
      "CreateDXGIFactory",
      "CreateDXGIFactory1",
      "CreateDXGIFactory2",
  };
  HANDLE proc = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                              FALSE, static_cast<DWORD>(pid));
  if (proc == nullptr) {
    // Module list already collected; prologue stage degrades gracefully.
    PrologueFinding pf;
    pf.export_name = "(prologue stage skipped)";
    pf.detail = "OpenProcess for prologue reads failed; module list only.";
    r.prologues.push_back(pf);
    r.scan_ok = true;
    return r;
  }

  // Build remote module ranges for detour-target ownership.
  std::vector<ModuleRange> ranges;
  {
    HMODULE mods[4096] = {};
    DWORD needed = 0;
    if (::EnumProcessModulesEx(proc, mods, sizeof(mods), &needed,
                               LIST_MODULES_ALL)) {
      const std::size_t count =
          static_cast<std::size_t>(needed) / sizeof(HMODULE);
      for (std::size_t i = 0; i < count; ++i) {
        MODULEINFO mi = {};
        if (!::GetModuleInformation(proc, mods[i], &mi, sizeof(mi))) continue;
        wchar_t path[32768] = {};
        if (::GetModuleFileNameExW(proc, mods[i], path,
                                   static_cast<DWORD>(std::size(path))) == 0) {
          continue;
        }
        ModuleRange mr;
        mr.path = WideToUtf8(path);
        mr.basename_lower = BasenameLower(mr.path);
        mr.base = reinterpret_cast<std::uint64_t>(mi.lpBaseOfDll);
        mr.size = static_cast<std::uint64_t>(mi.SizeOfImage);
        ranges.push_back(mr);
      }
    }
  }

  // Local dxgi reference + remote dxgi base.
  HMODULE local_dxgi = ::GetModuleHandleW(L"dxgi.dll");
  if (local_dxgi == nullptr) {
    local_dxgi = ::LoadLibraryW(L"dxgi.dll");
  }
  std::uint64_t remote_dxgi_base = 0;
  for (const auto& mr : ranges) {
    if (mr.basename_lower == "dxgi.dll") {
      remote_dxgi_base = mr.base;
      break;
    }
  }
  const bool self = (pid == static_cast<std::uint32_t>(::GetCurrentProcessId()));

  for (const char* name : kExports) {
    PrologueFinding pf;
    pf.export_name = name;
    if (local_dxgi == nullptr) {
      pf.detail = "local dxgi.dll unavailable; skipped.";
      r.prologues.push_back(pf);
      continue;
    }
    FARPROC local_fn = ::GetProcAddress(local_dxgi, name);
    if (local_fn == nullptr) {
      pf.detail = "export not found locally; skipped.";
      r.prologues.push_back(pf);
      continue;
    }
    std::uint64_t target = 0;
    if (self) {
      target = reinterpret_cast<std::uint64_t>(local_fn);
    } else {
      if (remote_dxgi_base == 0 ||
          !RemoteExportAddress(proc,
                               reinterpret_cast<HMODULE>(remote_dxgi_base),
                               local_fn, local_dxgi, target)) {
        pf.detail = "remote dxgi.dll base/RVA unavailable; skipped.";
        r.prologues.push_back(pf);
        continue;
      }
    }
    unsigned char bytes[16] = {};
    SIZE_T got = 0;
    bool read_ok = false;
    if (self) {
      // Own committed code: plain copy (no cross-process read needed).
      for (std::size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] =
            reinterpret_cast<const unsigned char*>(target)[i];  // NOLINT
      }
      got = sizeof(bytes);
      read_ok = true;
    } else {
      read_ok = ::ReadProcessMemory(proc,
                                    reinterpret_cast<LPCVOID>(target), bytes,
                                    sizeof(bytes), &got) != FALSE &&
                got == sizeof(bytes);
    }
    pf.remote_read_ok = read_ok;
    if (!read_ok) {
      char buf[128] = {};
      std::snprintf(buf, sizeof(buf), "remote prologue read failed gle=%lu",
                    static_cast<unsigned long>(::GetLastError()));
      pf.detail = buf;
      r.prologues.push_back(pf);
      continue;
    }
    std::string reason;
    pf.detour_like =
        LooksLikeHookedPrologue(bytes, sizeof(bytes), reason);
    std::ostringstream os;
    os << "bytes[" << HexBytes(bytes, sizeof(bytes)) << "] ";
    if (pf.detour_like) {
      std::uint64_t dest = 0;
      bool have_dest = false;
      if (bytes[0] == 0xE9 && sizeof(bytes) >= 5) {
        std::int32_t rel = 0;
        rel |= static_cast<std::int32_t>(bytes[1]);
        rel |= static_cast<std::int32_t>(bytes[2]) << 8;
        rel |= static_cast<std::int32_t>(bytes[3]) << 16;
        rel |= static_cast<std::int32_t>(bytes[4]) << 24;
        dest = target + 5 + static_cast<std::uint64_t>(
                                       static_cast<std::int64_t>(rel));
        have_dest = true;
      }
      os << "DETOUR-LIKE(" << reason << ")";
      std::string owner;
      if (have_dest && !self) owner = OwnerOf(ranges, dest);
      if (have_dest && self) {
        // Self-scan: attribute via local module snapshot.
        HMODULE owner_mod = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(dest), &owner_mod);
        if (owner_mod != nullptr) {
          wchar_t op[32768] = {};
          ::GetModuleFileNameW(owner_mod, op,
                               static_cast<DWORD>(std::size(op)));
          owner = WideToUtf8(op);
        }
      }
      if (have_dest) {
        char addr[64] = {};
        std::snprintf(addr, sizeof(addr), " jmp-target=0x%llx",
                      static_cast<unsigned long long>(dest));
        os << addr;
        if (!owner.empty()) {
          os << " owner=" << owner;
          DetourSuspicion ds;
          ds.export_name = name;
          ds.owner_path = owner;
          const std::string ob = BasenameLower(owner);
          const bool owner_is_dxgi = (ob == "dxgi.dll");
          ds.reason = owner_is_dxgi
                          ? "thunk lands inside dxgi.dll (likely benign)"
                          : "thunk lands in third-party module (suspicious)";
          r.detour_notes.push_back(ds);
          os << " (" << ds.reason << ")";
        } else {
          os << " owner=unknown";
          DetourSuspicion ds;
          ds.export_name = name;
          ds.reason = "JMP target outside all enumerated modules";
          r.detour_notes.push_back(ds);
        }
      } else {
        DetourSuspicion ds;
        ds.export_name = name;
        ds.reason = std::string("thunk-shaped entry (") + reason +
                    "); target not decoded";
        r.detour_notes.push_back(ds);
      }
    } else {
      os << "clean(entry=" << (reason.empty() ? "normal" : reason.c_str())
         << ")";
    }
    pf.detail = os.str();
    r.prologues.push_back(pf);
  }
  ::CloseHandle(proc);
  r.scan_ok = true;
  return r;
}

ConflictScanResult ScanSelf() {
  return ScanPid(static_cast<std::uint32_t>(::GetCurrentProcessId()), "");
}

std::string ConflictScanResult::ToText() const {
  std::ostringstream os;
  os << "MECVR T7 conflict scan (BEST-EFFORT, never exhaustive)\n";
  os << "pid=" << pid << " scan_ok=" << (scan_ok ? 1 : 0);
  if (!error.empty()) os << " error=" << error;
  os << "\n";
  os << "[modules] count=" << modules.size() << "\n";
  for (const auto& m : modules) {
    os << "  " << (m.from_game_dir ? "[game-dir] " : "[system]  ") << m.path
       << "\n";
  }
  os << "[known-hits] count=" << known_hits.size()
     << " (basename list only; absence proves nothing)\n";
  for (const auto& k : known_hits) {
    os << "  " << k.category << ": " << k.path << "\n";
  }
  if (known_hits.empty()) {
    os << "  none of the known basenames matched (best-effort only)\n";
  }
  os << "[dxgi-prologues]\n";
  for (const auto& p : prologues) {
    os << "  " << p.export_name
       << " read_ok=" << (p.remote_read_ok ? 1 : 0)
       << " detour_like=" << (p.detour_like ? 1 : 0) << " " << p.detail
       << "\n";
  }
  os << "[detours] notes=" << detour_notes.size() << "\n";
  for (const auto& d : detour_notes) {
    os << "  " << d.export_name << ": " << d.reason;
    if (!d.owner_path.empty()) os << " [" << d.owner_path << "]";
    os << "\n";
  }
  os << "scope-note: warnings are advisory only and never block the caller; "
        "only observed instability triggers STOP S2. Present/ResizeBuffers "
        "are vtable methods: no remote vtable resolution attempted; export "
        "prologue shape is a proxy signal, not a verdict.\n";
  return os.str();
}

}  // namespace mecvr::compat::scan
