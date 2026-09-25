cmake_minimum_required(VERSION 3.24)
project(MECVR VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# First-party warning policy: strict. Never apply /WX to vendored third-party code.
if(MSVC)
  add_compile_options(/W4 /WX)
else()
  add_compile_options(-Wall -Wextra -Werror)
endif()

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
