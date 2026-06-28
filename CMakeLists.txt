cmake_minimum_required(VERSION 3.16)
project(aunsync VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OBS_PLUGIN_LEGACY_INSTALL "Use legacy obs-plugins/64bit install layout" ON)

if(NOT DEFINED OBS_SOURCE_DIR)
  message(FATAL_ERROR "OBS_SOURCE_DIR not set.")
endif()

file(TO_CMAKE_PATH "${OBS_SOURCE_DIR}" OBS_DIR)

# Find OBS deps directory (version-independent)
file(GLOB _DEPS "${OBS_DIR}/.deps/obs-deps-*-x64")
if(_DEPS)
  list(GET _DEPS 0 OBS_DEPS_DIR)
  message(STATUS "OBS deps: ${OBS_DEPS_DIR}")
endif()

# ---- Resolve libobs dependencies ----
list(APPEND CMAKE_PREFIX_PATH
  "${OBS_DIR}/build_x64"
  "${OBS_DIR}/build_x64/libobs"
  "${OBS_DIR}/build_x64/deps/w32-pthreads"
  "${OBS_DIR}/build_x64/deps/libcaption"
  "${OBS_DEPS_DIR}"
  "${OBS_DEPS_DIR}/lib/cmake"
  "${OBS_DIR}/cmake"
)
list(APPEND CMAKE_MODULE_PATH
  "${OBS_DIR}/cmake/finders"
  "${OBS_DIR}/cmake"
)

find_package(w32-pthreads QUIET HINTS "${OBS_DIR}/build_x64/deps/w32-pthreads" "${OBS_DIR}/build_x64")
find_package(SIMDe QUIET   HINTS "${OBS_DEPS_DIR}" "${OBS_DEPS_DIR}/lib/cmake")
find_package(Threads REQUIRED)
find_package(libobs REQUIRED HINTS "${OBS_DIR}/build_x64/libobs" "${OBS_DIR}/build_x64" "${OBS_DIR}/cmake")

# ---- Qt (for custom properties widgets) ----
file(GLOB _QT6_DEPS "${OBS_DIR}/.deps/obs-deps-qt6-*-x64")
if(_QT6_DEPS)
  list(GET _QT6_DEPS 0 _QT6_DEP_DIR)
  list(APPEND CMAKE_PREFIX_PATH "${_QT6_DEP_DIR}")
endif()
find_package(Qt6 COMPONENTS Widgets QUIET)
if(NOT Qt6_FOUND)
  find_package(Qt5 5.15 COMPONENTS Widgets REQUIRED)
endif()

# ---- Plugin target ----
add_library(aunsync MODULE
  src/plugin/plugin-main.cpp
  src/plugin/plugin-utils.cpp
  src/plugin/plugin-services.cpp
  src/plugin/release-check.cpp
  src/plugin/plugin-config.cpp
  src/plugin/plugin-settings.cpp
  src/plugin/plugin-helpers.cpp
  src/audio/audio-processor.cpp
  src/audio/probe-signal.cpp
  src/ui/props-refresh.cpp
  src/ui/properties-delay.cpp
  src/ui/properties-builder.cpp
  src/widgets/delay-diagram-widget.cpp
  src/widgets/help-callout-widget.cpp
  src/widgets/widget-payload-utils.cpp
  src/network/rtsp-e2e-prober.cpp
)

if(OBS_DEPS_DIR)
  target_include_directories(aunsync PRIVATE
    "${OBS_DEPS_DIR}/include"
  )
  target_link_directories(aunsync PRIVATE
    "${OBS_DEPS_DIR}/lib"
  )
endif()

target_include_directories(aunsync BEFORE PRIVATE
  src/
)

# OBS include dirs come AFTER (added via target_link_libraries OBS::libobs)
target_link_libraries(aunsync
  OBS::libobs
  wininet
  iphlpapi
  psapi
)

target_compile_definitions(aunsync PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  SIMDE_NO_NATIVE
  PLUGIN_VERSION="${PROJECT_VERSION}"
)

# ---- Qt custom widgets ----
if(Qt6_FOUND)
  target_link_libraries(aunsync Qt6::Widgets)
else()
  target_link_libraries(aunsync Qt5::Widgets)
endif()

# Force MSVC to treat source as UTF-8
if(MSVC)
  target_compile_options(aunsync PRIVATE /utf-8 /wd4005 /wd4819)
endif()

if(OBS_PLUGIN_LEGACY_INSTALL)
  set(OBS_PLUGIN_BIN_DIR "obs-plugins/64bit")
  set(OBS_PLUGIN_DATA_DIR "data/obs-plugins/aunsync")
else()
  set(OBS_PLUGIN_BIN_DIR "plugins/aunsync/bin/64bit")
  set(OBS_PLUGIN_DATA_DIR "plugins/aunsync/data")
endif()

install(TARGETS aunsync LIBRARY DESTINATION "${OBS_PLUGIN_BIN_DIR}")
install(DIRECTORY data/locale/ DESTINATION "${OBS_PLUGIN_DATA_DIR}/locale")
install(FILES LICENSE THIRD_PARTY_NOTICES README.md DESTINATION .)

# Copy data files next to DLL so OBS finds them during development
add_custom_command(TARGET aunsync POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:aunsync>/data/obs-plugins/aunsync/locale"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/data/locale"
    "$<TARGET_FILE_DIR:aunsync>/data/obs-plugins/aunsync/locale"
)
