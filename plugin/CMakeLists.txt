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
    SeedlatheDesigner.h
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

# The font resource, for the formats iPlug2 leaves it off.
#
# _iplug_create_desktop_targets in Scripts/cmake/IPlugPlugin.cmake appends
# resources/main.rc to the APP target and builds every other format from
# ${sources} unchanged -- its comment reads "APP target (uses add_executable,
# needs .rc on Windows)", which is true of the app and not exclusive to it. On
# Windows IGraphics loads fonts from the calling module's own resources, so a
# plugin DLL built without the .rc has no font at all.
#
# It fails silently and it looks nothing like a missing resource: panels,
# knobs and meters all draw, and only the text is absent, because LoadFont
# returns false and nothing checks it. The standalone is unaffected, which
# makes it look like a host problem rather than a build one.
#
# The RC include paths have to match what iPlug2 sets for the APP, because
# source file properties are directory-scoped and this would otherwise
# overwrite them for the app target too.
if(WIN32)
  set(SEEDLATHE_RC "${CMAKE_CURRENT_SOURCE_DIR}/resources/main.rc")
  set_source_files_properties("${SEEDLATHE_RC}" PROPERTIES
    COMPILE_FLAGS "/I\"${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/resources/img\" /I\"${CMAKE_CURRENT_SOURCE_DIR}/resources\""
  )
  foreach(SEEDLATHE_FMT vst3 clap)
    if(TARGET Seedlathe-${SEEDLATHE_FMT})
      target_sources(Seedlathe-${SEEDLATHE_FMT} PRIVATE "${SEEDLATHE_RC}")
    endif()
  endforeach()
endif()
