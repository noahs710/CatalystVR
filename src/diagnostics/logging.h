#pragma once

// Minimal thread-safe file logger for diagnostics/support bundles.
#include <mutex>
#include <string>

namespace mecvr::diagnostics {

enum class Level { kInfo, kWarning, kError };

class Logger {
 public:
  static Logger& instance();
  bool open(const std::string& path);
  void log(Level level, const std::string& message);
  void close();

 private:
  Logger() = default;
  std::mutex mutex_;
  void* stream_ = nullptr;  // std::ofstream*, pimpl to keep header light.
};

}  // namespace mecvr::diagnostics
