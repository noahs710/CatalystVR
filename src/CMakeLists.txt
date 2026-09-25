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

add_library(mecvr_compat STATIC
  compatibility/support_report.cc
)
target_include_directories(mecvr_compat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_compat PUBLIC mecvr_core version psapi bcrypt)

# Interim M0A diagnostics CLI (game stream, T2). The real MECVR.exe launcher
# arrives with a later task.
add_executable(mecvr_diag bootstrap/diag_main.cc)
target_link_libraries(mecvr_diag PRIVATE mecvr_compat mecvr_core)
