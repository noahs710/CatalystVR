add_executable(mecvr_smoke smoke/smoke_main.cc)
target_link_libraries(mecvr_smoke PRIVATE mecvr_core)
add_test(NAME foundation_smoke COMMAND mecvr_smoke)

add_executable(mecvr_input_tests ../src/input/input_tests_main.cc)
target_link_libraries(mecvr_input_tests PRIVATE mecvr_input mecvr_core)
add_test(NAME input_tests COMMAND mecvr_input_tests)

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

add_executable(mecvr_synthetic_test ../src/render/synthetic_test.cc)
target_link_libraries(mecvr_synthetic_test PRIVATE mecvr_render)
add_test(NAME synthetic_observer_test COMMAND mecvr_synthetic_test)

add_executable(mecvr_conflict_test ../src/compatibility/conflict_scan_test.cc)
target_link_libraries(mecvr_conflict_test PRIVATE mecvr_conflict mecvr_compat mecvr_core)
add_test(NAME conflict_scanner_test COMMAND mecvr_conflict_test)
