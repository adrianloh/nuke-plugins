cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
project(Loki CXX)

# --- C++17 baseline ---------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Match the runtime Nuke itself uses on Windows. Must be set before
# any target is added.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# --- macOS deployment target ------------------------------------------
# Nuke 15.2 targets macOS 10.15+. Must be set before any target is added.

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
endif()

# --- Nuke NDK ---------------------------------------------------------
# NUKE_ROOT must point at the directory that contains cmake/ and include/.
# For a local Windows install this is e.g. C:/Program Files/Nuke15.2v9.
# In CI it points at the extracted NDK tarball.

if(NOT NUKE_ROOT)
    message(FATAL_ERROR
        "NUKE_ROOT not set. Pass -DNUKE_ROOT=<path-to-nuke-ndk-root>.\n"
        "  Windows local: C:/Program Files/Nuke15.2v9\n"
        "  CI: the extracted NDK tarball subdirectory for the current platform")
endif()

list(APPEND CMAKE_PREFIX_PATH "${NUKE_ROOT}")
find_package(Nuke REQUIRED)

# --- Helper to add one Loki plugin ------------------------------------
# Loki plugins have no external dependencies beyond the Nuke NDK and the
# C++ standard library — pure text-processing nodes.

function(add_loki_plugin TARGET_NAME SOURCE_FILE)
    # add_nuke_plugin() comes from NukeConfig.cmake. It:
    #   - creates an add_library(... MODULE)
    #   - strips the "lib" prefix
    #   - sets .dylib suffix on mac
    #   - adds NOMINMAX and _USE_MATH_DEFINES on Windows
    #   - links Nuke::NDK and its transitive deps
    add_nuke_plugin(${TARGET_NAME} ${SOURCE_FILE})

    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /W3 /EHsc)
    endif()

    if(APPLE)
        # Hide all symbols except the plugin description Nuke looks up.
        set_target_properties(${TARGET_NAME} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
        )
    endif()
endfunction()

# --- Plugin targets ---------------------------------------------------

add_loki_plugin(LokiEncode  LokiEncode.cpp)
add_loki_plugin(LokiDecode  LokiDecode.cpp)
add_loki_plugin(LokiShuffle LokiShuffle.cpp)
add_loki_plugin(LokiMerge   LokiMerge.cpp)
add_loki_plugin(LokiNoise   LokiNoise.cpp)
add_loki_plugin(LokiRemove  LokiRemove.cpp)
add_loki_plugin(LokiSort    LokiSort.cpp)
add_loki_plugin(LokiFilter  LokiFilter.cpp)

# --- Install ----------------------------------------------------------

if(NOT INSTALL_DIR)
    if(WIN32)
        set(INSTALL_DIR "$ENV{USERPROFILE}/.nuke" CACHE PATH "Plugin install directory")
    else()
        set(INSTALL_DIR "$ENV{HOME}/.nuke" CACHE PATH "Plugin install directory")
    endif()
endif()

install(TARGETS
    LokiEncode
    LokiDecode
    LokiShuffle
    LokiMerge
    LokiNoise
    LokiRemove
    LokiSort
    LokiFilter
    LIBRARY DESTINATION "${INSTALL_DIR}"
    RUNTIME DESTINATION "${INSTALL_DIR}"
)

message(STATUS "")
message(STATUS "Loki configuration:")
message(STATUS "  Platform:       ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Compiler:       ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  NUKE_ROOT:      ${NUKE_ROOT}")
message(STATUS "  Install dir:    ${INSTALL_DIR}")
message(STATUS "  Plugins:        8")
message(STATUS "")
