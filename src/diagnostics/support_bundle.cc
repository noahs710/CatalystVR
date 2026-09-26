// MECVR support-bundle builder implementation (plan T12, test stream).
// See support_bundle.h for the layout contract. No game, headset, or
// camera/stereo/gameplay code anywhere in this module (STOP S3 clean).

#include "diagnostics/support_bundle.h"

#include <windows.h>

#include <dxgi1_2.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "diagnostics/logging.h"
#include "mecvr_version.h"

namespace mecvr::diagnostics {
namespace {

namespace fs = std::filesystem;

void Log(const std::string& message) {
  Logger::instance().log(Level::kInfo, message);
}

std::string TimestampedName() {
  SYSTEMTIME st = {};
  GetLocalTime(&st);
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf), "mecvr-support-%04u%02u%02u-%02u%02u%02u",
                static_cast<unsigned>(st.wYear),
                static_cast<unsigned>(st.wMonth),
                static_cast<unsigned>(st.wDay),
                static_cast<unsigned>(st.wHour),
                static_cast<unsigned>(st.wMinute),
                static_cast<unsigned>(st.wSecond));
  return std::string(buf);
}

std::string ToUtf8(const wchar_t* wide) {
  if (wide == nullptr || *wide == L'\0') return std::string("(none)");
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                           nullptr, nullptr);
  if (needed <= 1) return std::string("(unconvertible)");
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr,
                        nullptr);
  return out;
}

std::string HexU32(unsigned long value) {
  char buf[16] = {};
  std::snprintf(buf, sizeof(buf), "0x%04lx", value);
  return std::string(buf);
}

std::string LuidText(const LUID& luid) {
  char buf[32] = {};
  std::snprintf(buf, sizeof(buf), "0x%08lx:0x%08lx",
                static_cast<unsigned long>(luid.HighPart),
                static_cast<unsigned long>(luid.LowPart));
  return std::string(buf);
}

// Milestone evidence docs (repo-relative under docs/). Missing files are
// recorded in the manifest, never fabricated.
const char* kEvidenceDocs[] = {
    "BASELINE_M0A.md", "OBSERVER_M0C1.md", "OBSERVER_M0C2.md",
    "HOOK_EVIDENCE.md", "INPUT.md",        "MOCKXR_M1A.md",
    "OPENXR_M1B.md",   "XRSCENE_M1C.md",   "COMPATIBILITY.md",
    "INJECTION_M0B.md", "GAME_BUILDS.md",  "TEST_MATRIX.md",
    "RELEASE.md",      "M2_RECORD.md",
};

bool CopyOne(const fs::path& src, const fs::path& dst, std::string* error) {
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  if (ec) {
    if (error != nullptr) *error = "mkdir failed: " + dst.string();
    return false;
  }
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    if (error != nullptr) *error = "copy failed: " + src.string();
    return false;
  }
  return true;
}

std::string FileSizeText(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec) return "unknown-size";
  return std::to_string(static_cast<unsigned long long>(size)) + " bytes";
}

void WriteBuildVersions(const fs::path& path) {
  std::ofstream out(path);
  out << "mecvr_version=" << MECVR_VERSION_MAJOR << "."
      << MECVR_VERSION_MINOR << "." << MECVR_VERSION_PATCH << "-"
      << MECVR_VERSION_SUFFIX << "\n";
  out << "msc_ver=" << _MSC_VER << "\n";
  out << "cplusplus=" << __cplusplus << "\n";
  out << "build_date=" << __DATE__ << " " << __TIME__ << "\n";
  out << "git_revision=not-collected (T12 collects no git state)\n";
}

// dxdiag-lite: OS + memory + DXGI adapters. No headset, no game, no D3D
// device creation — pure enumeration.
void WriteDxdiagLite(const fs::path& path, std::ostream* manifest) {
  std::ofstream out(path);

  // OS version via RtlGetVersion (GetVersionEx is deprecated/faked).
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  OSVERSIONINFOW os = {};
  os.dwOSVersionInfoSize = sizeof(os);
  if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        ::GetProcAddress(ntdll, "RtlGetVersion"));
    if (fn != nullptr) fn(&os);  // Leaves zeros when unavailable.
  }
  out << "[os]\n";
  out << "major=" << os.dwMajorVersion << " minor=" << os.dwMinorVersion
      << " build=" << os.dwBuildNumber << " platform=" << os.dwPlatformId
      << "\n";

  SYSTEM_INFO sys = {};
  ::GetNativeSystemInfo(&sys);
  out << "arch=" << sys.wProcessorArchitecture << " processors="
      << sys.dwNumberOfProcessors << "\n";

  MEMORYSTATUSEX mem = {};
  mem.dwLength = sizeof(mem);
  if (::GlobalMemoryStatusEx(&mem)) {
    out << "phys_total_mb="
        << (static_cast<unsigned long long>(mem.ullTotalPhys) >> 20) << "\n";
  }

  out << "[dxgi_adapters]\n";
  IDXGIFactory1* factory = nullptr;
  HRESULT hr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(&factory));
  if (FAILED(hr) || factory == nullptr) {
    out << "factory_error=0x" << std::hex
        << static_cast<unsigned long>(hr) << std::dec << "\n";
    if (manifest != nullptr) {
      *manifest << "dxdiag_lite: adapter enumeration unavailable (hr logged)\n";
    }
    return;
  }
  UINT index = 0;
  int gpu_count = 0;
  for (;;) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    if (adapter != nullptr) {
      DXGI_ADAPTER_DESC1 desc = {};
      if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        ++gpu_count;
        out << "adapter" << index
            << " description=\"" << ToUtf8(desc.Description) << "\""
            << " vendor=" << HexU32(desc.VendorId)
            << " device=" << HexU32(desc.DeviceId)
            << " subsys=" << HexU32(desc.SubSysId)
            << " revision=" << desc.Revision
            << " dedicated_video_mb="
            << (static_cast<unsigned long long>(desc.DedicatedVideoMemory) >>
                20)
            << " dedicated_system_mb="
            << (static_cast<unsigned long long>(
                    desc.DedicatedSystemMemory) >>
                20)
            << " shared_system_mb="
            << (static_cast<unsigned long long>(desc.SharedSystemMemory) >>
                20)
            << " luid=" << LuidText(desc.AdapterLuid)
            << " flags=" << desc.Flags << "\n";
      }
      adapter->Release();
    }
    ++index;
  }
  factory->Release();
  out << "adapter_count=" << gpu_count << "\n";
  if (manifest != nullptr) {
    *manifest << "dxdiag_lite: " << gpu_count << " DXGI adapter(s), no "
              << "headset required, no D3D device created\n";
  }
}

}  // namespace

BundleResult CreateSupportBundle(const BundleOptions& options) {
  BundleResult result;
  if (options.out_dir.empty()) {
    result.error = "--out DIR is required";
    return result;
  }

  const fs::path repo =
      options.repo_root.empty()
          ? fs::current_path()
          : fs::path(options.repo_root);
  const fs::path bundle =
      fs::path(options.out_dir) / TimestampedName();

  std::error_code ec;
  fs::create_directories(bundle, ec);
  if (ec) {
    result.error = "cannot create bundle dir: " + bundle.string();
    return result;
  }
  result.bundle_dir = bundle.string();

  Logger::instance().open((bundle / "support_bundle.log").string());
  Log("bundle started repo=" + repo.string());

  std::ostringstream manifest;
  manifest << "MECVR support bundle manifest\n";
  manifest << "tool=mecvr_support.exe (T12 support-bundle builder)\n";
  manifest << "format=plain directory bundle + MANIFEST.txt (no zip "
              "vendored per T12 scope; zip the folder with any tool)\n";
  manifest << "repo_root=" << repo.string() << "\n";
  manifest << "bundle=" << bundle.string() << "\n";

  // 1. Build versions.
  WriteBuildVersions(bundle / "build_versions.txt");
  manifest << "build_versions.txt: written\n";
  ++result.files_copied;
  Log("build versions written");

  // 2. dxdiag-lite.
  WriteDxdiagLite(bundle / "dxdiag_lite.txt", &manifest);
  ++result.files_copied;
  Log("dxdiag-lite written");

  // 3. Evidence docs.
  manifest << "[evidence]\n";
  for (const char* name : kEvidenceDocs) {
    const fs::path src = repo / "docs" / name;
    if (fs::exists(src, ec)) {
      std::string copy_error;
      if (CopyOne(src, bundle / "evidence" / name, &copy_error)) {
        manifest << "copied evidence/" << name << " ("
                 << FileSizeText(src) << ")\n";
        ++result.files_copied;
      } else {
        manifest << "ERROR " << copy_error << "\n";
        ++result.items_missing;
      }
    } else {
      manifest << "absent docs/" << name << " (not yet written)\n";
      ++result.items_missing;
    }
  }
  Log("evidence sweep done");

  // 4. ctest log (default under the repo; never runs ctest itself).
  manifest << "[ctest]\n";
  fs::path ctest_src = options.ctest_log_path.empty()
                           ? (repo / "build" / "Testing" / "Temporary" /
                              "LastTest.log")
                           : fs::path(options.ctest_log_path);
  if (fs::exists(ctest_src, ec)) {
    std::string copy_error;
    if (CopyOne(ctest_src, bundle / "ctest_lasttest.log", &copy_error)) {
      manifest << "copied ctest_lasttest.log from " << ctest_src.string()
               << " (" << FileSizeText(ctest_src) << ")\n";
      ++result.files_copied;
    } else {
      manifest << "ERROR " << copy_error << "\n";
      ++result.items_missing;
    }
  } else {
    manifest << "absent " << ctest_src.string()
             << " (ctest has not produced a log here)\n";
    ++result.items_missing;
  }

  // 5. Config (only when explicitly supplied).
  manifest << "[config]\n";
  if (!options.config_path.empty() && fs::exists(options.config_path, ec)) {
    std::string copy_error;
    if (CopyOne(options.config_path, bundle / "config" /
                                            fs::path(options.config_path)
                                                .filename(),
                &copy_error)) {
      manifest << "copied config from " << options.config_path << "\n";
      ++result.files_copied;
    } else {
      manifest << "ERROR " << copy_error << "\n";
      ++result.items_missing;
    }
  } else {
    manifest << "no live config supplied (--config PATH not given or "
                "missing); no default config collected\n";
    ++result.items_missing;
  }

  // 6. Logs: explicit --log files plus %TEMP%/mecvr_*.log sweep.
  manifest << "[logs]\n";
  for (const std::string& log : options.extra_logs) {
    if (!log.empty() && fs::exists(log, ec)) {
      std::string copy_error;
      if (CopyOne(log, bundle / "logs" / fs::path(log).filename(),
                  &copy_error)) {
        manifest << "copied log " << log << " (" << FileSizeText(log)
                 << ")\n";
        ++result.files_copied;
      } else {
        manifest << "ERROR " << copy_error << "\n";
        ++result.items_missing;
      }
    } else {
      manifest << "absent log " << log << "\n";
      ++result.items_missing;
    }
  }
  char temp_dir[MAX_PATH] = {};
  if (::GetTempPathA(sizeof(temp_dir), temp_dir) != 0) {
    std::error_code iter_ec;
    for (const auto& entry :
         fs::directory_iterator(temp_dir, iter_ec)) {
      if (iter_ec) break;
      const std::string name = entry.path().filename().string();
      if (name.rfind("mecvr_", 0) != 0) continue;
      if (entry.path().extension() == ".log") {
        std::string copy_error;
        if (CopyOne(entry.path(), bundle / "logs" / entry.path().filename(),
                    &copy_error)) {
          manifest << "swept temp log " << name << " ("
                   << FileSizeText(entry.path()) << ")\n";
          ++result.files_copied;
        } else {
          manifest << "ERROR " << copy_error << "\n";
          ++result.items_missing;
        }
      } else {
        manifest << "noted temp artifact (not copied) " << name << " ("
                 << FileSizeText(entry.path()) << ")\n";
      }
    }
  }
  Log("log sweep done");

  ++result.files_copied;  // Count MANIFEST.txt itself before summarizing.
  manifest << "[summary]\nfiles_copied=" << result.files_copied
           << " items_missing=" << result.items_missing << "\n";
  manifest << "note: missing = optional artifact absent; recorded, not "
              "fatal. Never read as a clean bill.\n";

  {
    std::ofstream out(bundle / "MANIFEST.txt");
    out << manifest.str();
  }

  result.manifest_path = (bundle / "MANIFEST.txt").string();
  result.ok = true;
  Log("bundle complete dir=" + result.bundle_dir);
  Logger::instance().close();
  return result;
}

}  // namespace mecvr::diagnostics
