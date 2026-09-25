add_executable(mecvr_smoke smoke/smoke_main.cc)
target_link_libraries(mecvr_smoke PRIVATE mecvr_core)
add_test(NAME foundation_smoke COMMAND mecvr_smoke)
