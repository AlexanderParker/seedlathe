add_executable(sl_tests
  test_smoke.cpp
  test_layering.cpp
  test_mulberry32.cpp
  test_presetio.cpp
  test_instrumentgen.cpp
  test_waparam.cpp
  test_waoscillator.cpp
  test_wabiquad.cpp
  test_wanodes.cpp
  test_wacompressor.cpp
  test_sharedfxrack.cpp
  test_voice.cpp
  test_offlinerender.cpp
)

target_link_libraries(sl_tests PRIVATE sl_core Catch2::Catch2WithMain)
target_compile_options(sl_tests PRIVATE ${SEEDLATHE_WARNINGS})

target_compile_definitions(sl_tests PRIVATE
  SL_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  SL_VECTORS_DIR="${CMAKE_SOURCE_DIR}/vectors")

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(sl_tests)
