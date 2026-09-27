// P1.0 foundation smoke test: proves test-exe generation, CTest discovery,
// Release config, runtime linkage, and the frozen config/logging surface.
#include <cstdio>
#include <filesystem>
#include <string>

#include "config/config.h"
#include "diagnostics/logging.h"
#include "mecvr_version.h"

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", name);
  } else {
    std::printf("ok: %s\n", name);
  }
}
}  // namespace

int main() {
  Check(MECVR_VERSION_MAJOR == 1 && MECVR_VERSION_UPDATE == 5 &&
            MECVR_VERSION_PATCH == 2 && MECVR_VERSION_MINOR_CHANGE == 1,
        "version surface present");

  mecvr::config::Registry registry;
  Check(registry.registerSection("mecvr", 1), "register core section");
  Check(!registry.registerSection("mecvr", 1), "duplicate section rejected");
  Check(registry.set("mecvr", "stereo_mode", "mono"), "config set");
  Check(!registry.set("unregistered", "k", "v"), "unregistered set rejected");
  const auto value = registry.get("mecvr", "stereo_mode");
  Check(value.has_value() && *value == "mono", "config get round-trip");
  Check(!registry.get("mecvr", "missing").has_value(), "missing key empty");

  const auto tmp =
      std::filesystem::temp_directory_path() / "mecvr_smoke_config.txt";
  Check(registry.save(tmp.string()), "config save");
  mecvr::config::Registry reloaded;
  Check(reloaded.registerSection("mecvr", 1), "register before load");
  Check(reloaded.load(tmp.string()), "config load");
  const auto back = reloaded.get("mecvr", "stereo_mode");
  Check(back.has_value() && *back == "mono", "config file round-trip");
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  Check(!ec, "temp config cleanup");

  const auto log_path =
      std::filesystem::temp_directory_path() / "mecvr_smoke.log";
  auto& logger = mecvr::diagnostics::Logger::instance();
  Check(logger.open(log_path.string()), "logger open");
  logger.log(mecvr::diagnostics::Level::kInfo, "smoke");
  logger.close();
  Check(std::filesystem::exists(log_path, ec), "log file written");
  std::filesystem::remove(log_path, ec);

  if (failures == 0) std::printf("smoke: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
