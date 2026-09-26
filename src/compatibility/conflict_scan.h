#pragma once

// T7 compatibility scanner (game stream): BEST-EFFORT conflict detection.
//
// This is a best-effort heuristic scan, NEVER an exhaustive detector. It
// cannot prove the absence of unknown Present hooks; absence of findings
// means "none of the known signatures matched", not "no hooks exist".
// Findings are advisory warnings: the API returns them and the CALLER
// decides what to do. Warnings never block by themselves; only observed
// real instability (hitch/crash/regression vs baseline) triggers STOP S2.
//
// Read-only by design: EnumProcessModules + GetModuleFileNameEx +
// bounded ReadProcessMemory of a few export prologue bytes. No injection,
// no hooks, no WriteProcessMemory, no CreateRemoteThread, no game writes.
//
// Reuses mecvr::compat::ModuleEntry / OverlayFinding from
// compatibility/support_report.h (T2) for module + known-hit shapes;
// enumeration itself is reimplemented here for arbitrary PIDs because
// support_report.cc keeps its collector in an anonymous namespace
// (those files are intentionally NOT modified).
#include <cstdint>
#include <string>
#include <vector>

#include "compatibility/support_report.h"

namespace mecvr::compat::scan {

// One inspected DXGI export prologue (local and, when possible, remote).
struct PrologueFinding {
  std::string export_name;
  bool remote_read_ok = false;
  bool detour_like = false;  // true => JMP-thunk shaped prologue observed.
  std::string detail;        // human-readable evidence (bytes + ownership).
};

// A jump-thunk target whose owning module looks third-party/suspicious.
struct DetourSuspicion {
  std::string export_name;
  std::string owner_path;  // module owning the JMP target, if identified.
  std::string reason;
};

struct ConflictScanResult {
  bool scan_ok = false;
  std::uint32_t pid = 0;
  std::string error;
  std::vector<ModuleEntry> modules;
  std::vector<OverlayFinding> known_hits;  // best-effort basename matches.
  std::vector<PrologueFinding> prologues;
  std::vector<DetourSuspicion> detour_notes;
  bool HasWarnings() const {
    return !known_hits.empty() || !detour_notes.empty();
  }
  std::string ToText() const;
};

// Enumerate loaded modules of an arbitrary live PID (read-only).
// Returns entries; sets error on failure (empty list + error text).
std::vector<ModuleEntry> EnumerateProcessModules(std::uint32_t pid,
                                                 const std::string& game_dir,
                                                 std::string& error);

// Best-effort basename matcher over an already-enumerated module list.
// Never exhaustive: unknown proxies/shims are NOT detectable this way.
std::vector<OverlayFinding> MatchKnownModules(
    const std::vector<ModuleEntry>& modules);

// Full scan of one PID: module list + known-basename match + best-effort
// DXGI export-prologue inspection (bounded remote reads only).
// game_dir is used ONLY to flag game-dir shim suspects (dxgi/d3d11/dinput8
// loaded from the game dir instead of System32/WinSxS); pass "" to skip.
ConflictScanResult ScanPid(std::uint32_t pid, const std::string& game_dir);

// Scan the calling process (self-test path; same code as ScanPid).
ConflictScanResult ScanSelf();

// Pure helper, unit-testable: does this prologue look like a JMP-thunk
// detour (E9 rel32 / EB rel8 / FF 25 indirect)? reason set on true.
bool LooksLikeHookedPrologue(const unsigned char* bytes, std::size_t len,
                             std::string& reason);

}  // namespace mecvr::compat::scan
