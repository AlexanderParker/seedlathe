add_executable(sl_tests
  test_smoke.cpp
  test_layering.cpp
)

target_link_libraries(sl_tests PRIVATE sl_core Catch2::Catch2WithMain)
target_compile_options(sl_tests PRIVATE ${SEEDLATHE_WARNINGS})

target_compile_definitions(sl_tests PRIVATE
  SL_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  SL_VECTORS_DIR="${CMAKE_SOURCE_DIR}/vectors")

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(sl_tests)
