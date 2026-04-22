cmake_minimum_required(VERSION 3.16 FATAL_ERROR)
project(BaseSixFour CXX)

# --- C++17 baseline ---------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
# We build a static libwebp so the plugin has zero runtime dependency on
# Homebrew / system libwebp. Users drop the plugin in ~/.nuke and it works.

include(FetchContent)

set(WEBP_BUILD_ANIM_UTILS   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP        OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP        OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP        OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS       OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX   OFF CACHE BOOL "" FORCE)
set(WEBP_LINK_STATIC        ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libwebp
    GIT_REPOSITORY https://chromium.googlesource.com/webm/libwebp
    GIT_TAG        v1.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libwebp)

# --- Plugin target ----------------------------------------------------
# add_nuke_plugin() is provided by NukeConfig.cmake. It:
#   - creates an add_library(... MODULE)
#   - strips the "lib" prefix
#   - sets .dylib suffix on mac
#   - adds NOMINMAX and _USE_MATH_DEFINES on Windows
#   - links Nuke::NDK and its transitive deps

add_nuke_plugin(BaseSixFour
    BaseSixFour.cpp
    stb_impl.cpp
)

target_include_directories(BaseSixFour PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(BaseSixFour PRIVATE
    webp
)

# --- Platform-specific polish -----------------------------------------

if(MSVC)
    # Match the runtime Nuke itself uses on Windows.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    target_compile_options(BaseSixFour PRIVATE /W3 /EHsc)
endif()

if(APPLE)
    # Hide all symbols except the plugin description that Nuke looks up.
    # Not strictly required, but avoids symbol collisions between plugins.
    set_target_properties(BaseSixFour PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

# --- Install ----------------------------------------------------------

if(NOT INSTALL_DIR)
    if(WIN32)
        set(INSTALL_DIR "$ENV{USERPROFILE}/.nuke" CACHE PATH "Plugin install directory")
    else()
        set(INSTALL_DIR "$ENV{HOME}/.nuke" CACHE PATH "Plugin install directory")
    endif()
endif()

install(TARGETS BaseSixFour
    LIBRARY DESTINATION "${INSTALL_DIR}"
    RUNTIME DESTINATION "${INSTALL_DIR}"
)

message(STATUS "")
message(STATUS "BaseSixFour configuration:")
message(STATUS "  Platform:       ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Compiler:       ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  NUKE_ROOT:      ${NUKE_ROOT}")
message(STATUS "  Install dir:    ${INSTALL_DIR}")
message(STATUS "")
