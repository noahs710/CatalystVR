add_executable(mecvr_smoke smoke/smoke_main.cc)
target_link_libraries(mecvr_smoke PRIVATE mecvr_core)
add_test(NAME foundation_smoke COMMAND mecvr_smoke)

add_executable(mecvr_input_tests ../src/input/input_tests_main.cc)
target_link_libraries(mecvr_input_tests PRIVATE mecvr_input mecvr_core)
add_test(NAME input_tests COMMAND mecvr_input_tests)

add_executable(mecvr_mock_xr_test ../src/openxr/mock_xr_test.cc)
target_link_libraries(mecvr_mock_xr_test PRIVATE mecvr_openxr_mock)
add_test(NAME mock_xr_test COMMAND mecvr_mock_xr_test)

add_executable(mecvr_synthetic_test ../src/render/synthetic_test.cc)
target_link_libraries(mecvr_synthetic_test PRIVATE mecvr_render)
add_test(NAME synthetic_observer_test COMMAND mecvr_synthetic_test)

add_executable(mecvr_conflict_test ../src/compatibility/conflict_scan_test.cc)
target_link_libraries(mecvr_conflict_test PRIVATE mecvr_conflict mecvr_compat mecvr_core)
add_test(NAME conflict_scanner_test COMMAND mecvr_conflict_test)
