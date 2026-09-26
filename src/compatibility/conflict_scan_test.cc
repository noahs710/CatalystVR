// T7 conflict-scan test: self-scan + synthetic matcher/prologue unit checks.
// Optional live-game mode: conflict_scan_test.exe --pid <PID> [game_dir]
// performs a read-only ScanPid of that process and prints the report.
// Exit code: 0 = all checks pass, 1 = any failure.
#include <windows.h>

#include <cstdio>
#include <string>

#include "compatibility/conflict_scan.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* name) {
  std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", name);
  if (!cond) ++g_failures;
}

void UnitMatcherChecks() {
  using mecvr::compat::ModuleEntry;
  using mecvr::compat::scan::MatchKnownModules;
  std::vector<ModuleEntry> mods;
  auto add = [&](const std::string& path, bool gamedir) {
    ModuleEntry e;
    e.path = path;
    const std::size_t s = path.find_last_of("\\/");
    std::string b = s == std::string::npos ? path : path.substr(s + 1);
    for (char& c : b)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    e.basename_lower = b;
    e.from_game_dir = gamedir;
    mods.push_back(e);
  };
  add("C:\\Sys\\kernel32.dll", false);
  add("C:\\Tools\\ReShade64.dll", false);
  add("C:\\Tools\\RTSSHooks64.dll", false);
  add("C:\\Tools\\SpecialK64.dll", false);
  add("C:\\Windows\\System32\\gameoverlayrenderer64.dll", false);
  add("C:\\Games\\MEC\\dxgi.dll", true);          // game-dir shim: flag.
  add("C:\\Windows\\System32\\dxgi.dll", false);  // system copy: skip.
  add("C:\\Games\\MEC\\dinput8.dll", true);       // game-dir shim: flag.
  const auto hits = MatchKnownModules(mods);
  bool reshade = false, rtss = false, sk = false, steam = false,
       shim_dxgi = false, shim_dinput = false, sys_dxgi = false;
  for (const auto& h : hits) {
    if (h.basename_lower == "reshade64.dll") reshade = true;
    if (h.basename_lower == "rtsshooks64.dll") rtss = true;
    if (h.basename_lower == "specialk64.dll") sk = true;
    if (h.basename_lower == "gameoverlayrenderer64.dll") steam = true;
    if (h.basename_lower == "dxgi.dll" && h.path == "C:\\Games\\MEC\\dxgi.dll")
      shim_dxgi = true;
    if (h.basename_lower == "dxgi.dll" &&
        h.path == "C:\\Windows\\System32\\dxgi.dll")
      sys_dxgi = true;
    if (h.basename_lower == "dinput8.dll") shim_dinput = true;
  }
  Check(reshade, "matcher flags reshade64.dll");
  Check(rtss, "matcher flags rtsshooks64.dll");
  Check(sk, "matcher flags specialk64.dll");
  Check(steam, "matcher flags gameoverlayrenderer64.dll");
  Check(shim_dxgi, "matcher flags game-dir dxgi.dll shim");
  Check(shim_dinput, "matcher flags game-dir dinput8.dll shim");
  Check(!sys_dxgi, "matcher ignores System32 dxgi.dll");
}

void UnitPrologueChecks() {
  using mecvr::compat::scan::LooksLikeHookedPrologue;
  std::string reason;
  const unsigned char jmp[] = {0xE9, 0x11, 0x22, 0x33, 0x44, 0x90};
  Check(LooksLikeHookedPrologue(jmp, sizeof(jmp), reason),
        "prologue flags E9 JMP rel32");
  const unsigned char short_jmp[] = {0xEB, 0xFE, 0x90, 0x90};
  Check(LooksLikeHookedPrologue(short_jmp, sizeof(short_jmp), reason),
        "prologue flags EB short-JMP at entry");
  const unsigned char ind[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
  Check(LooksLikeHookedPrologue(ind, sizeof(ind), reason),
        "prologue flags FF 25 indirect-JMP");
  const unsigned char pushret[] = {0x68, 0x11, 0x22, 0x33, 0x44, 0xC3};
  Check(LooksLikeHookedPrologue(pushret, sizeof(pushret), reason),
        "prologue flags PUSH+RET stub");
  const unsigned char normal[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57};
  Check(!LooksLikeHookedPrologue(normal, sizeof(normal), reason),
        "prologue passes normal mov prologue");
  Check(!LooksLikeHookedPrologue(nullptr, 0, reason),
        "prologue handles null input without detour claim");
}

}  // namespace

int main(int argc, char** argv) {
  std::uint32_t pid_arg = 0;
  std::string game_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--pid" && i + 1 < argc) {
      pid_arg = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--gamedir" && i + 1 < argc) {
      game_dir = argv[++i];
    }
  }

  UnitMatcherChecks();
  UnitPrologueChecks();

  // Self-scan: same code path as remote scan, exercised on own process.
  const auto self = mecvr::compat::scan::ScanSelf();
  Check(self.scan_ok, "self-scan completes");
  Check(!self.modules.empty(), "self-scan enumerates own modules");
  Check(!self.prologues.empty(), "self-scan inspects dxgi prologues");
  bool self_kernel32 = false;
  for (const auto& m : self.modules) {
    if (m.basename_lower == "kernel32.dll") self_kernel32 = true;
  }
  Check(self_kernel32, "self-scan sees kernel32.dll");
  std::printf("--- self-scan report ---\n%s",
              self.ToText().c_str());

  // Optional live-PID scan (read-only; caller owns launch/terminate).
  if (pid_arg != 0) {
    const auto live = mecvr::compat::scan::ScanPid(pid_arg, game_dir);
    std::printf("--- live pid=%u report ---\n%s", static_cast<unsigned>(pid_arg),
                live.ToText().c_str());
    Check(live.scan_ok, "live-pid scan completes");
    Check(!live.modules.empty(), "live-pid scan enumerates modules");
  }

  if (g_failures == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("%d CHECK(S) FAILED\n", g_failures);
  return 1;
}
