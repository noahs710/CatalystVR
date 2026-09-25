#pragma once

// Frozen core config interface (T1). Later modules (e.g. input bindings in
// src/config/input_bindings.*) register their own sections here; the
// loader/writer, versioning, and section registry below must not change
// shape without a plan amendment.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mecvr::config {

inline constexpr int kConfigVersion = 1;

class Registry {
 public:
  // Registers a named section owned by a module. Returns false if already
  // registered. Sections must be registered before load() will accept them.
  bool registerSection(const std::string& name, int version);

  bool set(const std::string& section, const std::string& key,
           const std::string& value);
  std::optional<std::string> get(const std::string& section,
                                 const std::string& key) const;
  std::vector<std::string> sections() const;

  // Persists as "# mecvr-config v<kConfigVersion>" followed by
  // "[section]" / "key=value" lines with "#" comments allowed on load.
  bool save(const std::string& path) const;
  bool load(const std::string& path);

 private:
  std::map<std::string, int> section_versions_;
  std::map<std::string, std::map<std::string, std::string>> values_;
};

}  // namespace mecvr::config
