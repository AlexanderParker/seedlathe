if(NOT DEFINED IPLUG2_DIR)
  set(IPLUG2_DIR "${CMAKE_SOURCE_DIR}/third_party/iPlug2" CACHE PATH "iPlug2 root directory")
endif()

include(${IPLUG2_DIR}/iPlug2.cmake)
find_package(iPlug2 REQUIRED)

# NanoVG is the default IGraphics backend and needs no prebuilt-lib download.
# Skia is a P2/P3 decision, once there is a UI worth the dependency weight.
iplug_add_plugin(Seedlathe
  SOURCES
    Seedlathe.cpp
    Seedlathe.h
    config.h
    SeedlatheControls.h
    SeedlatheParams.h
    resources/resource.h
  RESOURCES
    resources/fonts/Roboto-Regular.ttf
  FORMATS
    APP
    VST3
    CLAP
  LINK
    iPlug2::Extras::Synth
    sl_core
)
