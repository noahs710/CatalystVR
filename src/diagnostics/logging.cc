#include "diagnostics/logging.h"

#include <fstream>

namespace mecvr::diagnostics {

namespace {
const char* ToLabel(Level level) {
  switch (level) {
    case Level::kInfo:
      return "INFO";
    case Level::kWarning:
      return "WARN";
    case Level::kError:
      return "ERROR";
  }
  return "UNKNOWN";
}
}  // namespace

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

bool Logger::open(const std::string& path) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto* out = new std::ofstream(path, std::ios::app);
  if (!*out) {
    delete out;
    return false;
  }
  delete static_cast<std::ofstream*>(stream_);
  stream_ = out;
  return true;
}

void Logger::log(Level level, const std::string& message) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto* out = static_cast<std::ofstream*>(stream_);
  if (out != nullptr) {
    *out << "[" << ToLabel(level) << "] " << message << "\n";
    out->flush();
  }
}

void Logger::close() {
  const std::lock_guard<std::mutex> lock(mutex_);
  delete static_cast<std::ofstream*>(stream_);
  stream_ = nullptr;
}

}  // namespace mecvr::diagnostics
