add_executable(mecvr_smoke smoke/smoke_main.cc)
target_link_libraries(mecvr_smoke PRIVATE mecvr_core)
add_test(NAME foundation_smoke COMMAND mecvr_smoke)

add_executable(mecvr_input_tests ../src/input/input_tests_main.cc)
target_link_libraries(mecvr_input_tests PRIVATE mecvr_input mecvr_core)
add_test(NAME input_tests COMMAND mecvr_input_tests)

add_executable(mecvr_motion_scheme_test input/motion_scheme_test_main.cc)
target_link_libraries(mecvr_motion_scheme_test PRIVATE mecvr_input mecvr_core)
add_test(NAME motion_scheme_test COMMAND mecvr_motion_scheme_test)

add_executable(mecvr_comfort_test input/comfort_test_main.cc)
target_link_libraries(mecvr_comfort_test PRIVATE mecvr_input mecvr_core)
add_test(NAME comfort_filter_test COMMAND mecvr_comfort_test)

add_executable(mecvr_recenter_test input/recenter_test_main.cc)
target_include_directories(mecvr_recenter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME recenter_latch_test COMMAND mecvr_recenter_test)

add_executable(mecvr_mock_xr_test ../src/openxr/mock_xr_test.cc)
target_link_libraries(mecvr_mock_xr_test PRIVATE mecvr_openxr_mock)
add_test(NAME mock_xr_test COMMAND mecvr_mock_xr_test)

add_executable(mecvr_real_xr_test ../src/openxr/real_xr_test.cc)
target_link_libraries(mecvr_real_xr_test PRIVATE mecvr_openxr_real mecvr_openxr_mock)
# The test loads the vendored loader DLL; stage it beside the test binary.
add_custom_command(TARGET mecvr_real_xr_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/bin/openxr_loader.dll
    $<TARGET_FILE_DIR:mecvr_real_xr_test>
)
add_test(NAME real_xr_session_test COMMAND mecvr_real_xr_test)

add_executable(mecvr_xr_scene_test xr_scene/xr_scene_test.cc)
target_link_libraries(mecvr_xr_scene_test PRIVATE mecvr_xr_scene mecvr_openxr_real mecvr_openxr_mock)
add_custom_command(TARGET mecvr_xr_scene_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/bin/openxr_loader.dll
    $<TARGET_FILE_DIR:mecvr_xr_scene_test>
)
add_test(NAME xr_scene_test COMMAND mecvr_xr_scene_test)

add_executable(mecvr_m2b_test m2b/m2b_mono_test_main.cc)
target_link_libraries(mecvr_m2b_test PRIVATE mecvr_m2b mecvr_openxr_mock winmm)
add_test(NAME m2b_mono_transport_test COMMAND mecvr_m2b_test)

add_executable(mecvr_m2b_matrix_test m2b/m2b_matrix_test_main.cc)
target_link_libraries(mecvr_m2b_matrix_test PRIVATE mecvr_m2b mecvr_openxr_mock)
add_test(NAME m2b_rate_matrix_test COMMAND mecvr_m2b_matrix_test)

add_executable(mecvr_stereo_epoch_test m2b/stereo_epoch_test_main.cc)
target_include_directories(mecvr_stereo_epoch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME stereo_epoch_gate_test COMMAND mecvr_stereo_epoch_test)

add_executable(mecvr_stereo_eye_test m2b/stereo_eye_test_main.cc)
target_include_directories(mecvr_stereo_eye_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME stereo_eye_classifier_test COMMAND mecvr_stereo_eye_test)

add_executable(mecvr_shared_capture_mailbox_test
  m2b/shared_capture_mailbox_test_main.cc)
target_include_directories(mecvr_shared_capture_mailbox_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME shared_capture_mailbox_test
  COMMAND mecvr_shared_capture_mailbox_test)

# T10G adapter/LUID gate tool. PASS and OPEN both satisfy CTest via the
# verdict line; only FAIL (LUID mismatch) fails the test.
add_executable(mecvr_t10g_gate m2b/t10g_gate_main.cc)
target_include_directories(mecvr_t10g_gate PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/include
)
target_link_libraries(mecvr_t10g_gate PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/lib/openxr_loader.lib
  d3d11 dxgi
)
add_custom_command(TARGET mecvr_t10g_gate POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr/bin/openxr_loader.dll
    $<TARGET_FILE_DIR:mecvr_t10g_gate>
)
add_test(NAME t10g_adapter_gate COMMAND mecvr_t10g_gate)
set_tests_properties(t10g_adapter_gate PROPERTIES
  PASS_REGULAR_EXPRESSION "T10G_GATE_VERDICT: (PASS|OPEN)"
)

add_executable(mecvr_synthetic_test ../src/render/synthetic_test.cc)
target_link_libraries(mecvr_synthetic_test PRIVATE mecvr_render)
add_test(NAME synthetic_observer_test COMMAND mecvr_synthetic_test)

add_executable(mecvr_conflict_test ../src/compatibility/conflict_scan_test.cc)
target_link_libraries(mecvr_conflict_test PRIVATE mecvr_conflict mecvr_compat mecvr_core)
add_test(NAME conflict_scanner_test COMMAND mecvr_conflict_test)

add_executable(mecvr_camera_math_test camera/camera_math_test_main.cc)
target_link_libraries(mecvr_camera_math_test PRIVATE mecvr_camera)
add_test(NAME camera_math_test COMMAND mecvr_camera_math_test)

add_executable(mecvr_camera_harness_test camera/camera_harness_test_main.cc)
target_link_libraries(mecvr_camera_harness_test PRIVATE mecvr_camera)
add_test(NAME camera_harness_test COMMAND mecvr_camera_harness_test)

add_executable(mecvr_view_override_test camera/view_override_test_main.cc)
target_link_libraries(mecvr_view_override_test PRIVATE mecvr_camera)
add_test(NAME view_override_test COMMAND mecvr_view_override_test)

add_executable(mecvr_arm_ik_test ik/arm_ik_test_main.cc)
target_link_libraries(mecvr_arm_ik_test PRIVATE mecvr_ik)
add_test(NAME arm_ik_test COMMAND mecvr_arm_ik_test)

add_executable(mecvr_full_body_ik_test ik/full_body_ik_test_main.cc)
target_link_libraries(mecvr_full_body_ik_test PRIVATE mecvr_ik)
add_test(NAME full_body_ik_test COMMAND mecvr_full_body_ik_test)

add_executable(mecvr_parkour_intent_test
  ik/parkour_intent_test_main.cc)
target_link_libraries(mecvr_parkour_intent_test PRIVATE mecvr_ik)
add_test(NAME parkour_intent_test COMMAND mecvr_parkour_intent_test)

add_executable(mecvr_native_bone_map_test
  ik/native_bone_map_test_main.cc)
target_link_libraries(mecvr_native_bone_map_test PRIVATE mecvr_ik mecvr_render)
add_test(NAME native_bone_map_test COMMAND mecvr_native_bone_map_test)

add_executable(mecvr_native_pose_writer_test
  ik/native_pose_writer_test_main.cc)
target_link_libraries(mecvr_native_pose_writer_test PRIVATE mecvr_ik mecvr_render)
add_test(NAME native_pose_writer_test COMMAND mecvr_native_pose_writer_test)

add_executable(mecvr_mod_owned_integration_test
  ik/mod_owned_integration_test_main.cc
  ../src/render/body_overlay_geometry.cc)
target_include_directories(mecvr_mod_owned_integration_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(mecvr_mod_owned_integration_test PRIVATE mecvr_ik mecvr_input)
add_test(NAME mod_owned_integration_test COMMAND mecvr_mod_owned_integration_test)

add_executable(mecvr_body_overlay_geometry_test
  ik/body_overlay_geometry_test_main.cc
  ../src/render/body_overlay_geometry.cc)
target_include_directories(mecvr_body_overlay_geometry_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(mecvr_body_overlay_geometry_test PRIVATE mecvr_ik)
add_test(NAME body_overlay_geometry_test COMMAND mecvr_body_overlay_geometry_test)

add_executable(mecvr_body_overlay_render_test
  ik/body_overlay_render_test_main.cc
  ../src/render/body_overlay.cc
  ../src/render/body_overlay_geometry.cc)
target_include_directories(mecvr_body_overlay_render_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(mecvr_body_overlay_render_test PRIVATE
  mecvr_ik d3d11 d3dcompiler)
add_test(NAME body_overlay_render_test COMMAND mecvr_body_overlay_render_test)

add_executable(mecvr_bone_palette_classifier_test
  m3a/bone_palette_classifier_test_main.cc)
target_link_libraries(mecvr_bone_palette_classifier_test PRIVATE mecvr_render)
add_test(NAME bone_palette_classifier_test
  COMMAND mecvr_bone_palette_classifier_test)

add_executable(mecvr_native_skeleton_adapter_test
  m3a/native_skeleton_adapter_test_main.cc)
target_link_libraries(mecvr_native_skeleton_adapter_test PRIVATE mecvr_render)
add_test(NAME native_skeleton_adapter_test
  COMMAND mecvr_native_skeleton_adapter_test)

add_executable(mecvr_m3a_slots_test m3a/m3a_slots_main.cc)
target_link_libraries(mecvr_m3a_slots_test PRIVATE mecvr_render)
add_test(NAME m3a_slot_proof COMMAND mecvr_m3a_slots_test)

add_executable(mecvr_force_mode_test force_mode/force_mode_test_main.cc)
target_link_libraries(mecvr_force_mode_test PRIVATE mecvr_render)
add_test(NAME force_mode_test COMMAND mecvr_force_mode_test)

add_test(NAME immersive_defaults_test
  COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
          ${CMAKE_CURRENT_SOURCE_DIR}/launcher/immersive_defaults_test.ps1)

add_test(NAME early_injection_test
  COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
          ${CMAKE_CURRENT_SOURCE_DIR}/launcher/early_injection_test.ps1)

add_executable(mecvr_shared_blit_math_test
  m2b/shared_blit_math_test_main.cc)
target_include_directories(mecvr_shared_blit_math_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME shared_blit_math_test COMMAND mecvr_shared_blit_math_test)

add_executable(mecvr_shared_texture_contract_test
  m2b/shared_texture_contract_test_main.cc)
target_link_libraries(mecvr_shared_texture_contract_test PRIVATE d3d11 dxgi)
add_test(NAME shared_texture_contract_test
  COMMAND mecvr_shared_texture_contract_test)
