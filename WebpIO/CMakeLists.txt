cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
project(WebpIO CXX)

# --- C++17 baseline ---------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Match the runtime Nuke itself uses on Windows. Must be set before
# any target (including FetchContent'd libwebp) is added.
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

# --- libwebp (static, built from source) ------------------------------
# We need three components from libwebp:
#   - webp        encoder + decoder (always built)
#   - webpdemux   demuxer (parsing chunks like XMP, EXIF) — webpReader uses
#   - webpmux     muxer (writing chunks into the file) — webpWriter uses
#
# We disable all the optional command-line tools (cwebp, dwebp, etc.) so only
# the libraries get compiled. Total libwebp build is ~10s on a CI runner.

include(FetchContent)

set(WEBP_BUILD_ANIM_UTILS   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP        OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP        OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP        OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX      OFF CACHE BOOL "" FORCE)   # tool, not the lib
set(WEBP_BUILD_EXTRAS       OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX   ON  CACHE BOOL "" FORCE)   # the libwebpmux library
set(WEBP_LINK_STATIC        ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libwebp
    GIT_REPOSITORY https://chromium.googlesource.com/webm/libwebp
    GIT_TAG        v1.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libwebp)

# --- Helper to add one webp-using plugin ------------------------------

function(add_webp_plugin TARGET_NAME SOURCE_FILE)
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

    # Forward each plugin's libwebp deps in via ARGN
    target_link_libraries(${TARGET_NAME} PRIVATE ${ARGN})

    if(MSVC)
        # Match the runtime Nuke itself uses on Windows.
        target_compile_options(${TARGET_NAME} PRIVATE /W3 /EHsc)
    endif()

    if(APPLE)
        # Hide all symbols except the plugin description that Nuke looks up.
        set_target_properties(${TARGET_NAME} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
        )
    endif()
endfunction()

# --- Plugin targets ---------------------------------------------------
# webpReader: needs decoder + demuxer
# webpWriter: needs encoder + muxer
# Both 'webp' and the components are CMake targets exposed by libwebp's
# CMakeLists. Linking 'webp' alone gives both encoder and decoder symbols.

add_webp_plugin(webpReader webpReader.cpp
    webp
    webpdemux
)

add_webp_plugin(webpWriter webpWriter.cpp
    webp
    libwebpmux
)

# --- Install ----------------------------------------------------------

if(NOT INSTALL_DIR)
    if(WIN32)
        set(INSTALL_DIR "$ENV{USERPROFILE}/.nuke" CACHE PATH "Plugin install directory")
    else()
        set(INSTALL_DIR "$ENV{HOME}/.nuke" CACHE PATH "Plugin install directory")
    endif()
endif()

install(TARGETS webpReader webpWriter
    LIBRARY DESTINATION "${INSTALL_DIR}"
    RUNTIME DESTINATION "${INSTALL_DIR}"
)

message(STATUS "")
message(STATUS "WebpIO configuration:")
message(STATUS "  Platform:       ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Compiler:       ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  NUKE_ROOT:      ${NUKE_ROOT}")
message(STATUS "  Install dir:    ${INSTALL_DIR}")
message(STATUS "")
