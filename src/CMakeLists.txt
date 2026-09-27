# First-party targets only. third_party/ targets must set their own flags.
add_library(mecvr_core STATIC
  config/config.cc
  diagnostics/logging.cc
)
target_include_directories(mecvr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(mecvr_render STATIC
  render/vtable_hook.cc
  render/observer.cc
  render/force_mode.cc
  render/bone_palette_classifier.cc
  render/native_skeleton_adapter.cc
)
target_include_directories(mecvr_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_render PUBLIC d3d11 dxgi)

add_library(mecvr_input STATIC
  input/arbitration.cc
  input/comfort.cc
  input/motion_scheme.cc
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
target_link_libraries(mecvr_openxr_real PUBLIC mecvr_openxr_mock d3d11 dxgi d3dcompiler)
target_link_libraries(mecvr_openxr_real PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/lib/openxr_loader.lib
)

# Standalone stereo test scene (render/test streams, T9). Own D3D11 device,
# per-eye render targets, eye-tinted output verified by readback.
add_library(mecvr_xr_scene STATIC
  openxr/test/xr_scene.cc
)
target_include_directories(mecvr_xr_scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_xr_scene PUBLIC mecvr_openxr_mock d3d11 dxgi)

# M2B mono transport (render stream, T11). Identical mono image to both
# eyes; game thread never waits on XR. No camera/stereo/gameplay code.
add_library(mecvr_m2b STATIC
  render/m2b_mono.cc
  openxr/xr_frame_worker.cc
)
target_include_directories(mecvr_m2b PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_m2b PUBLIC mecvr_openxr_mock)

# P2.0 camera math (Sub-project 2). Headless: math, conventions,
# snapshot contract, body origin, hierarchy, harness. No game, no XR.
add_library(mecvr_camera STATIC
  camera/math.cc
  camera/conventions.cc
  camera/body_origin.cc
  camera/hierarchy.cc
  camera/harness.cc
  camera/view_override.cc
)
target_include_directories(mecvr_camera PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_camera PUBLIC mecvr_openxr_mock)

# Deterministic mod-owned upper-body pose layer. The library is independent
# of OpenXR and can be exercised entirely with synthetic controller poses.
add_library(mecvr_ik STATIC
  ik/arm_ik.cc
  ik/full_body_ik.cc
  ik/native_bone_map.cc
  ik/native_pose_writer.cc
  ik/height_calibration.cc
  ik/parkour_intent.cc
)
target_include_directories(mecvr_ik PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_ik PUBLIC mecvr_camera)

add_library(mecvr_compat STATIC
  compatibility/support_report.cc
)
target_include_directories(mecvr_compat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_compat PUBLIC mecvr_core version psapi bcrypt)

# Interim M0A diagnostics CLI (game stream, T2). The real MECVR.exe launcher
# arrives with a later task.
add_executable(mecvr_diag bootstrap/diag_main.cc)
target_link_libraries(mecvr_diag PRIVATE mecvr_compat mecvr_core)

# Support-bundle tool (test stream, T12). Directory bundle + manifest
# (no zip library vendored by decision).
add_executable(mecvr_support diagnostics/support_bundle_main.cc diagnostics/support_bundle.cc)
target_include_directories(mecvr_support PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_support PRIVATE mecvr_core dxgi)

# Native public launcher. It owns persisted configuration, package checks,
# dry-run validation, and the actual PowerShell launch handoff.
add_executable(mecvr_launcher WIN32 launcher/launcher_main.cc)
target_link_libraries(mecvr_launcher PRIVATE comdlg32 user32)

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

# M0C2 live-observation probe (render stream, T3B). Self-contained (own
# MT-compiled copies of the observer sources, like mecvr_core_mt) so the
# injected DLL never mixes CRTs. Observation only.
add_library(mecvr_live_probe SHARED
  render/live_probe.cc
  render/observer.cc
  render/vtable_hook.cc
)
target_include_directories(mecvr_live_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_live_probe PRIVATE d3d11 dxgi user32)
set_property(TARGET mecvr_live_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# M2A capture/HUD-boundary probe (render stream, T10). Same self-contained
# MT pattern. Observation only; HUD conversion forbidden.
add_library(mecvr_m2a_probe SHARED
  render/m2a_probe.cc
  render/observer.cc
  render/vtable_hook.cc
)
target_include_directories(mecvr_m2a_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_m2a_probe PRIVATE d3d11 dxgi user32)
set_property(TARGET mecvr_m2a_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# M3a observe-only CB intel probe (Sub-project 2). Self-contained MT DLL:
# context observation + fingerprints + bounded dumps. Zero writes.
add_library(mecvr_m3a_probe SHARED
  render/m3a_probe.cc
  render/bone_palette_classifier.cc
  render/native_skeleton_adapter.cc
  ik/parkour_intent.cc
  render/observer.cc
  render/vtable_hook.cc
)
target_include_directories(mecvr_m3a_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_m3a_probe PRIVATE d3d11 d3d12 dxgi user32)
set_property(TARGET mecvr_m3a_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# M3b opt-in camera bridge. It reuses the observe-only probe and adds a
# dedicated OpenXR pose worker plus a fail-closed view-matrix rewrite. The
# original m3a_probe target remains observe-only for all existing captures.
add_library(mecvr_m3b_live SHARED
  render/m3a_probe.cc
  render/body_overlay.cc
  render/body_overlay_geometry.cc
  render/bone_palette_classifier.cc
  render/native_skeleton_adapter.cc
  ik/parkour_intent.cc
  render/live_capture.cc
  render/observer.cc
  render/vtable_hook.cc
  camera/math.cc
  camera/view_override.cc
  ik/arm_ik.cc
  ik/full_body_ik.cc
  ik/native_bone_map.cc
  ik/native_pose_writer.cc
  ik/height_calibration.cc
  input/motion_scheme.cc
  input/comfort.cc
  input/game_input_synth.cc
  openxr/real_xr_backend.cc
  openxr/xr_frame_worker.cc
)
target_compile_definitions(mecvr_m3b_live PRIVATE MECVR_M3B=1)
target_include_directories(mecvr_m3b_live PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/include
)
target_link_libraries(mecvr_m3b_live PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/lib/openxr_loader.lib
  d3d11 d3d12 d3dcompiler dxgi user32
)
set_property(TARGET mecvr_m3b_live PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_custom_command(TARGET mecvr_m3b_live POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/bin/openxr_loader.dll
    $<TARGET_FILE_DIR:mecvr_m3b_live>
)

# M2B live mono submitter (render stream, T11-live). Self-contained MT DLL:
# capture + mailbox + real XR worker. Mono only — no camera, stereo,
# gameplay, or timing code.
add_library(mecvr_m2b_live SHARED
  render/m2b_live.cc
  render/m2b_mono.cc
  openxr/xr_frame_worker.cc
  render/observer.cc
  render/vtable_hook.cc
  openxr/real_xr_backend.cc
)
target_include_directories(mecvr_m2b_live PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/include
)
target_link_libraries(mecvr_m2b_live PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/lib/openxr_loader.lib
  d3d11 d3dcompiler dxgi user32
)
set_property(TARGET mecvr_m2b_live PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
# The injected DLL must resolve the OpenXR loader from its own directory.
add_custom_command(TARGET mecvr_m2b_live POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/bin/openxr_loader.dll
    $<TARGET_FILE_DIR:mecvr_m2b_live>
)

# Best-effort conflict scanner (game stream, T7). Findings only, never blocks.
add_library(mecvr_conflict STATIC compatibility/conflict_scan.cc)
target_include_directories(mecvr_conflict PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mecvr_conflict PUBLIC mecvr_compat mecvr_core psapi version)
