#pragma once

// MECVR support-bundle builder (plan T12, test stream).
//
// Collects config, logs, milestone evidence docs, build versions, the
// ctest log, and dxdiag-lite (OS/GPU/driver via DXGI, no headset
// required) into a timestamped bundle directory.
//
// BUNDLE FORMAT (documented choice): the bundle is a plain directory
// tree plus MANIFEST.txt — NOT a .zip. No zip library is vendored in
// this task (per the T12 brief); a directory bundle is inspectable
// without tooling and zips trivially on any machine. MANIFEST.txt
// records exactly what was collected and what was absent so a missing
// artifact is never mistaken for a clean result.
//
// Layout under <out>/mecvr-support-YYYYMMDD-HHMMSS/:
//   MANIFEST.txt          collection record (tool, time, gaps, notes)
//   build_versions.txt    MECVR version + compiler + build timestamp
//   dxdiag_lite.txt       OS + memory + DXGI adapter enumeration
//   ctest_lasttest.log    copy of the ctest log when present
//   evidence/             milestone docs copied from <repo>/docs
//   config/               --config file copy when supplied+present
//   logs/                 --log files + %TEMP%/mecvr_*.log when present
//
// New files only; diagnostics/logging.* are reused untouched.

#include <string>
#include <vector>

namespace mecvr::diagnostics {

struct BundleOptions {
  std::string repo_root;  // MECVR repo root (default: working directory).
  std::string out_dir;    // Parent dir receiving the bundle (required).
  std::vector<std::string> extra_logs;  // Extra log files to include.
  std::string config_path;  // Optional live config file to include.
  std::string ctest_log_path;  // Optional override; defaults to
                               // <repo>/build/Testing/Temporary/LastTest.log.
};

struct BundleResult {
  bool ok = false;
  std::string bundle_dir;
  std::string manifest_path;
  std::string error;
  int files_copied = 0;
  int items_missing = 0;  // Optional artifacts absent (recorded, not fatal).
};

BundleResult CreateSupportBundle(const BundleOptions& options);

}  // namespace mecvr::diagnostics
