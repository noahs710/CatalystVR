// Interim diagnostics CLI (T2, game stream). The real MECVR.exe launcher
// arrives with a later task; this tool only prints the M0A static/process
// baseline report. Usage:
//   mecvr_diag.exe --report [--game-dir PATH] [--no-launch] [--wait SECONDS]
#include <cstdio>
#include <string>

#include "compatibility/support_report.h"

namespace {

void PrintUsage() {
  std::puts(
      "usage: mecvr_diag.exe --report [--game-dir PATH] [--no-launch] "
      "[--wait SECONDS]");
}

}  // namespace

int main(int argc, char** argv) {
  bool report = false;
  bool do_launch = true;
  int wait_seconds = 120;
  std::string game_dir =
      "C:\\Users\\Gabrielle Monlea\\Downloads\\Mirrors Edge Catalyst";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--report") {
      report = true;
    } else if (arg == "--no-launch") {
      do_launch = false;
    } else if (arg == "--game-dir" && i + 1 < argc) {
      game_dir = argv[++i];
    } else if (arg == "--wait" && i + 1 < argc) {
      wait_seconds = std::atoi(argv[++i]);
      if (wait_seconds < 0) wait_seconds = 0;
      if (wait_seconds > 600) wait_seconds = 600;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::printf("unknown arg: %s\n", arg.c_str());
      PrintUsage();
      return 2;
    }
  }

  if (!report) {
    PrintUsage();
    return 2;
  }

  const mecvr::compat::SupportReport r =
      mecvr::compat::BuildStaticBaseline(game_dir, do_launch, wait_seconds);
  std::fputs(r.ToText().c_str(), stdout);
  return 0;
}
