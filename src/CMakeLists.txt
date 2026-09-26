# First-party targets only. third_party/ targets must set their own flags.
add_library(mecvr_core STATIC
  config/config.cc
  diagnostics/logging.cc
)
target_include_directories(mecvr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(mecvr_render STATIC
  render/vtable_hook.cc
  render/observer.cc
)
target_include_directories(mecvr_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_render PUBLIC d3d11 dxgi)

add_library(mecvr_input STATIC
  input/arbitration.cc
  input/openxr_device.cc
  input/xinput_device.cc
  config/input_bindings.cc
)
target_include_directories(mecvr_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_input PUBLIC mecvr_core xinput)

add_library(mecvr_openxr_mock STATIC
  openxr/mock_xr_backend.cc
)
target_include_directories(mecvr_openxr_mock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Real OpenXR backend (render stream, T8). Vendored Khronos loader lives in
# third_party/openxr (headers + lib + dll + loader source reference).
add_library(mecvr_openxr_real STATIC
  openxr/real_xr_backend.cc
)
target_include_directories(mecvr_openxr_real PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/include
)
target_link_libraries(mecvr_openxr_real PUBLIC mecvr_openxr_mock d3d11 dxgi)
target_link_libraries(mecvr_openxr_real PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/lib/openxr_loader.lib
)

add_library(mecvr_compat STATIC
  compatibility/support_report.cc
)
target_include_directories(mecvr_compat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_compat PUBLIC mecvr_core version psapi bcrypt)

# Interim M0A diagnostics CLI (game stream, T2). The real MECVR.exe launcher
# arrives with a later task.
add_executable(mecvr_diag bootstrap/diag_main.cc)
target_link_libraries(mecvr_diag PRIVATE mecvr_compat mecvr_core)

# M0B safe-injection runtime (game stream, T6). Static CRT: the injected DLL
# must not depend on a CRT DLL inside the game process.
# Static-CRT twin of mecvr_core: the injected DLL must not mix CRTs, so the
# objects linked into it are compiled /MT as well (same sources, same flags).
add_library(mecvr_core_mt STATIC
  config/config.cc
  diagnostics/logging.cc
)
target_include_directories(mecvr_core_mt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET mecvr_core_mt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(mecvr_runtime SHARED bootstrap/runtime.cc)
target_include_directories(mecvr_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_runtime PRIVATE mecvr_core_mt)
set_property(TARGET mecvr_runtime PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(mecvr_inject bootstrap/inject_main.cc bootstrap/inject.cc)
target_include_directories(mecvr_inject PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Best-effort conflict scanner (game stream, T7). Findings only, never blocks.
add_library(mecvr_conflict STATIC compatibility/conflict_scan.cc)
target_include_directories(mecvr_conflict PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_conflict PUBLIC mecvr_compat mecvr_core psapi version)
