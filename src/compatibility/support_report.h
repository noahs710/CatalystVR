#pragma once

// M0A static/process baseline (T2, game stream). Externally observable facts
// only: on-disk exe identity + live process/window/module survey. No D3D11
// device/swapchain/cadence claims (those belong to T3B).
#include <cstdint>
#include <string>
#include <vector>

namespace mecvr::compat {

// Fingerprint of one game executable read from disk (never executed).
struct ExeFingerprint {
  std::string path;
  std::string file_name;
  bool exists = false;
  std::uint64_t file_size = 0;
  std::string sha256;  // lowercase hex, empty on failure.
  std::string file_version;
  std::string product_version;
  std::string error;  // non-empty when exists but unreadable.
};

// One loaded module observed in the live game process.
struct ModuleEntry {
  std::string path;
  std::string basename_lower;
  bool from_game_dir = false;
};

// A known overlay/injector/proxy hit (best-effort basename match, T7 refines).
struct OverlayFinding {
  std::string basename_lower;
  std::string path;
  std::string category;  // e.g. "reshade", "rtss", "specialk", "proxy-suspect".
};

struct LiveProcessBaseline {
  bool attempted = false;
  bool found_window = false;
  std::uint32_t pid = 0;
  std::string parent_name;
  std::uint32_t parent_pid = 0;
  bool launched_by_tool = false;
  // Main window (externally observable via EnumWindows).
  std::uint64_t hwnd = 0;
  std::string window_title;
  std::string window_class;
  int client_w = 0;
  int client_h = 0;
  std::string window_mode;  // "windowed" | "fullscreen-borderless" | "unknown".
  std::string window_evidence;
  std::vector<ModuleEntry> modules;
  std::vector<OverlayFinding> overlay_findings;
  std::string launcher_note;
  std::string error;             // launch/wait failure recorded here.
  bool terminated_cleanly = false;
};

struct SupportReport {
  std::vector<ExeFingerprint> exes;
  LiveProcessBaseline live;
  std::string game_dir;
  std::string ToText() const;
};

// Hashes + versions both exes from disk. Never launches anything.
std::vector<ExeFingerprint> FingerprintExes(const std::string& game_dir);

// Launches retail exe (CreateProcess, no injection), waits up to
// wait_seconds for a visible main window, collects the baseline, then
// requests close and terminates on refusal. Game dir is never written.
LiveProcessBaseline CollectLiveBaseline(const std::string& retail_exe_path,
                                        int wait_seconds);

// Full static report: disk fingerprints always; live branch optional.
SupportReport BuildStaticBaseline(const std::string& game_dir, bool do_launch,
                                  int wait_seconds);

}  // namespace mecvr::compat
