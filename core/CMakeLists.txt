add_library(sl_core STATIC
  src/InstrumentGen.cpp
  src/PresetIO.cpp
  src/webaudio/WaParam.cpp
  src/webaudio/WaOscillator.cpp
  src/webaudio/WaBiquad.cpp
  src/webaudio/WaNodes.cpp
  src/webaudio/WaConvolver.cpp
  src/webaudio/WaCompressor.cpp
  src/SharedFxRack.cpp
  src/Voice.cpp
  src/RackPool.cpp
  src/SeedSearch.cpp
  src/OfflineRender.cpp
  src/Analysis.cpp
  src/WavIO.cpp
  src/SampleMatch.cpp
  src/UserPresets.cpp
)

target_include_directories(sl_core
  PUBLIC
    include
    src
)

target_link_libraries(sl_core PUBLIC nlohmann_json::nlohmann_json)

target_compile_features(sl_core PUBLIC cxx_std_20)
target_compile_options(sl_core PRIVATE ${SEEDLATHE_WARNINGS})
