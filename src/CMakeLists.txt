# First-party targets only. third_party/ targets must set their own flags.
add_library(mecvr_core STATIC
  config/config.cc
  diagnostics/logging.cc
)
target_include_directories(mecvr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
