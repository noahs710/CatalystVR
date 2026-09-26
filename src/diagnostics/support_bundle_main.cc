// mecvr_support.exe CLI (plan T12, test stream).
//
// Usage:
//   mecvr_support.exe --bundle --out DIR [--root REPO] [--log PATH]...
//                     [--config PATH] [--ctest PATH]
//
// No game, headset, or compilation-database dependency: runs against the
// repo tree as-is (no game needed). Exit 0 = bundle written (see
// MANIFEST.txt for gaps); 1 = bundle failure; 2 = usage error.

#include <iostream>
#include <string>

#include "diagnostics/support_bundle.h"

namespace {

void PrintUsage() {
  std::cout
      << "usage: mecvr_support.exe --bundle --out DIR [--root REPO]\n"
      << "                         [--log PATH]... [--config PATH] [--ctest PATH]\n"
      << "  --bundle      build a support bundle (required)\n"
      << "  --out DIR     parent dir receiving mecvr-support-<timestamp>/\n"
      << "  --root REPO   repo root to collect from (default: cwd)\n"
      << "  --log PATH    extra log file to include (repeatable)\n"
      << "  --config PATH live config file to include\n"
      << "  --ctest PATH  ctest log override (default: "
         "<repo>/build/Testing/Temporary/LastTest.log)\n";
}

}  // namespace

int main(int argc, char** argv) {
  mecvr::diagnostics::BundleOptions options;
  bool bundle = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--bundle") {
      bundle = true;
    } else if (arg == "--out" && i + 1 < argc) {
      options.out_dir = argv[++i];
    } else if (arg == "--root" && i + 1 < argc) {
      options.repo_root = argv[++i];
    } else if (arg == "--log" && i + 1 < argc) {
      options.extra_logs.push_back(argv[++i]);
    } else if (arg == "--config" && i + 1 < argc) {
      options.config_path = argv[++i];
    } else if (arg == "--ctest" && i + 1 < argc) {
      options.ctest_log_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::cout << "unknown argument: " << arg << "\n";
      PrintUsage();
      return 2;
    }
  }

  if (!bundle) {
    std::cout << "nothing to do: pass --bundle\n";
    PrintUsage();
    return 2;
  }
  if (options.out_dir.empty()) {
    std::cout << "--out DIR is required\n";
    PrintUsage();
    return 2;
  }

  const mecvr::diagnostics::BundleResult result =
      mecvr::diagnostics::CreateSupportBundle(options);
  if (!result.ok) {
    std::cout << "bundle FAILED: " << result.error << "\n";
    return 1;
  }
  std::cout << "bundle_dir=" << result.bundle_dir << "\n";
  std::cout << "manifest=" << result.manifest_path << "\n";
  std::cout << "files_copied=" << result.files_copied
            << " items_missing=" << result.items_missing << "\n";
  return 0;
}
