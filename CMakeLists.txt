cmake_minimum_required(VERSION 3.31)
project(Seedlathe VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# iPlug2 defaults to C++17 and assigns CMAKE_CXX_STANDARD itself when its
# module is included. Ask for 20 so the plugin matches core.
set(IPLUG2_CXX_STANDARD 20 CACHE STRING "C++ standard for iPlug2")

if(MSVC)
  # /fp:precise is load-bearing: /fp:fast lets MSVC reassociate floating-point
  # expressions, which breaks exact golden-vector equality against zyn.js.
  add_compile_options(/fp:precise)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
  # /W4 is applied per-target, not globally: iPlug2's own sources emit
  # thousands of C4100/C4244 warnings at that level and bury real ones.
  set(SEEDLATHE_WARNINGS /W4 /permissive-)
else()
  set(SEEDLATHE_WARNINGS -Wall -Wextra)
endif()

include(FetchContent)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2 nlohmann_json)

add_subdirectory(core)

option(SEEDLATHE_BUILD_PLUGIN "Build the iPlug2 plugin targets" ON)
if(SEEDLATHE_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()

enable_testing()
add_subdirectory(tests)
