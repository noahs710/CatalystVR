#include "config/config.h"

#include <fstream>
#include <sstream>

namespace mecvr::config {

namespace {
std::string Trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const auto begin = s.find_first_not_of(ws);
  if (begin == std::string::npos) return "";
  const auto end = s.find_last_not_of(ws);
  return s.substr(begin, end - begin + 1);
}
}  // namespace

bool Registry::registerSection(const std::string& name, int version) {
  return section_versions_.emplace(name, version).second;
}

bool Registry::set(const std::string& section, const std::string& key,
                   const std::string& value) {
  if (section_versions_.find(section) == section_versions_.end()) return false;
  values_[section][key] = value;
  return true;
}

std::optional<std::string> Registry::get(const std::string& section,
                                         const std::string& key) const {
  const auto sit = values_.find(section);
  if (sit == values_.end()) return std::nullopt;
  const auto kit = sit->second.find(key);
  if (kit == sit->second.end()) return std::nullopt;
  return kit->second;
}

std::vector<std::string> Registry::sections() const {
  std::vector<std::string> out;
  for (const auto& [name, version] : section_versions_) {
    (void)version;
    out.push_back(name);
  }
  return out;
}

bool Registry::save(const std::string& path) const {
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << "# mecvr-config v" << kConfigVersion << "\n";
  for (const auto& [section, version] : section_versions_) {
    (void)version;
    out << "[" << section << "]\n";
    const auto it = values_.find(section);
    if (it != values_.end()) {
      for (const auto& [key, value] : it->second) {
        out << key << "=" << value << "\n";
      }
    }
  }
  return static_cast<bool>(out);
}

bool Registry::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  std::string current;
  bool saw_version = false;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') {
      if (line == "# mecvr-config v1") saw_version = true;
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      current = line.substr(1, line.size() - 2);
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos || current.empty()) return false;
    if (section_versions_.find(current) == section_versions_.end()) {
      continue;  // Unknown section: skip, never fail (forward compatibility).
    }
    values_[current][Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
  }
  return saw_version;
}

}  // namespace mecvr::config
