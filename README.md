# nuke-plugins

Foundry Nuke NDK plugins. Builds for **Windows x86_64** and **macOS arm64**
in GitHub Actions. Compiled binaries committed under each plugin's
`Release/` directory.

This README is a notes-to-self / handover doc, not user-facing
documentation. The plugins themselves have their own per-directory
READMEs.

## TL;DR for future me

```bash
python build.py                  # list buildable plugins
python build.py BaseSixFour      # build one plugin (~3-5 min)
python build.py all              # build everything sequentially
python build.py BaseSixFour --download   # skip CI, just grab last successful
```

Then:

```bash
git status
git add */Release/
git commit -m "Rebuild binaries"
git push
```

If `build.py` says no token, drop the GitHub PAT into a folder named
`github_pat_<rest_of_token>` next to the script. It's gitignored.

---

## How this thing works

Two pieces:

1. **Cloud builds.** `python build.py <name>` triggers a GitHub Actions
   workflow that compiles the plugin on `windows-2022` (MSVC 2022) and
   `macos-14` (Apple Silicon). The workflow downloads our custom NDK
   tarball from S3, runs CMake + Ninja/MSBuild, uploads artifacts, and
   build.py drops them into `<plugin>/Release/<platform>/`.

2. **Local builds.** Optional. The same `CMakeLists.txt` works on a
   Windows machine with Nuke 15.2v9 installed (see "Local environment"
   below). Useful for fast iteration while writing a new plugin; cloud
   builds are the canonical artifacts.

The NDK tarball at
`https://smack.s3.ap-southeast-1.amazonaws.com/nuke-ndk-15.2v9.tar.gz`
contains only headers + Foundry's CMake config (~3 MB). No Nuke binaries.
On macOS, no `.dylib` files are needed because Foundry's mac NukeConfig
uses `-undefined,dynamic_lookup` and defers symbol resolution to Nuke at
plugin load time.

---

## Local environment (current)

What's on the dev machine where this all started, for reference:

- Windows 11
- Nuke 15.2v9 installed at `C:\Program Files\Nuke15.2v9`
- Visual Studio 2022 Enterprise, MSVC 14.42
- CMake 3.28.1
- Python 3.x with `requests` (for `build.py`)

To build a plugin locally:

```powershell
cd <plugin>
cmake -S . -B build `
  -DNUKE_ROOT="C:\Program Files\Nuke15.2v9" `
  -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output ends up in `build\Release\<name>.dll`. Sizes will differ slightly
from the cloud-built binaries — different MSVC patch version, different
build paths get baked in. Functionally identical.

The CMakeLists default install dir is `%USERPROFILE%\.nuke` on Windows
and `~/.nuke` on macOS, so:

```powershell
cmake --install build --config Release
```

drops the `.dll` straight into your local Nuke plugin path.

---

## Adding a new plugin

The whole pipeline is directory-driven. A "buildable plugin" is any
top-level directory containing a `CMakeLists.txt`. No registry to update,
no workflow file to edit, no dropdown to maintain.

```
nuke-plugins/
  MyNewThing/
    CMakeLists.txt
    MyNewThing.cpp
```

`python build.py` will see it immediately and let you build it via
`python build.py MyNewThing`.

The CMakeLists has to follow the cloud-buildable pattern — see the next
section.

---

## Cloud-ifying a Windows-only CMakeLists.txt

The recurring task. You usually start with a CMakeLists that finds Nuke
via a hardcoded `C:/Program Files/Nuke15.2v9` and does manual
`find_library(DDImage)`. Here's how to convert it.

### What to remove

Drop these — Foundry's `NukeConfig.cmake` handles them all on both
platforms:

```cmake
# REMOVE: manual NDK hunt
if(NOT NUKE_INSTALL_DIR)
    set(NUKE_INSTALL_DIR "C:/Program Files/Nuke15.2v9" ...)
endif()
find_library(DDIMAGE_LIBRARY NAMES DDImage HINTS "${NUKE_INSTALL_DIR}")

# REMOVE: Windows-only compile defines
target_compile_definitions(${TARGET_NAME} PRIVATE
    FN_OS_WINDOWS WIN32 _WIN32 NOMINMAX USE_GLEW _USE_MATH_DEFINES)

# REMOVE: hardcoded .dll suffix
set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "" SUFFIX ".dll")

# REMOVE: add_library(... SHARED ...)
add_library(${TARGET_NAME} SHARED ${SOURCE_FILE})
```

### What to add

```cmake
# C++17 baseline
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# MSVC runtime (must be set BEFORE any add_library, including FetchContent'd deps)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# macOS deployment target — Nuke 15.2 needs 10.15+
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "")
endif()

# Use Foundry's CMake config — provides Nuke::NDK and add_nuke_plugin()
if(NOT NUKE_ROOT)
    message(FATAL_ERROR "NUKE_ROOT not set")
endif()
list(APPEND CMAKE_PREFIX_PATH "${NUKE_ROOT}")
find_package(Nuke REQUIRED)

# Each plugin becomes one call to add_nuke_plugin().
# This handles: MODULE library, no "lib" prefix, .dylib suffix on mac,
# NOMINMAX/_USE_MATH_DEFINES on Windows, links Nuke::NDK + transitive deps.
add_nuke_plugin(MyPlugin MyPlugin.cpp)
```

### NUKE_ROOT — what it is on each platform

`NUKE_ROOT` is the directory that contains Foundry's `cmake/` and
`include/` subdirectories.

- **Windows local**: `C:/Program Files/Nuke15.2v9`
- **macOS local**:   `/Applications/Nuke15.2v9/Nuke15.2v9.app/Contents/MacOS`
- **CI**: the extracted NDK tarball subdir (`ndk/nuke-ndk-15.2v9/windows`
  or `ndk/nuke-ndk-15.2v9/macos_arm64` — the workflow picks the right one)

### Third-party dependencies

If your plugin needs an external library (libwebp, libpng, etc.) the
local CMakeLists usually `find_library`s it from a system path. Cloud
runners don't have those installed. Use **FetchContent** to build the
library from source as a static lib — then there's no runtime dep on
Homebrew/system libraries either, and the plugin is self-contained.

See `BaseSixFour/CMakeLists.txt` for the libwebp pattern. Skeleton:

```cmake
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WEBP_LINK_STATIC ON CACHE BOOL "" FORCE)
# ... disable optional tools so only the libraries get built ...

FetchContent_Declare(libwebp
    GIT_REPOSITORY https://chromium.googlesource.com/webm/libwebp
    GIT_TAG v1.4.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libwebp)

target_link_libraries(MyPlugin PRIVATE webp webpdemux libwebpmux)
```

Pin the `GIT_TAG` to a specific version. Don't use `main` — it makes
builds non-reproducible and can break overnight if upstream changes
target names (libwebp's `webpmux` vs `libwebpmux` rename in v1.2.3 is
the cautionary tale).

### Cross-platform source code

The `.cpp` files themselves are usually fine as-is if they only use
standard C++ and DDImage APIs. Watch for:

- Win32 file I/O (`_lseek`, `_telli64`, `_read`, `_close`) — needs a
  `#ifdef _WIN32` / `#else` with POSIX equivalents (`lseek`, `read`,
  `fstat`, `close`).
- `#include <io.h>` and `<fcntl.h>` — Windows-only, guard with `#ifdef
  _WIN32`.
- Anything that assumes path separators or environment variable names.

Minor cosmetic warnings on macOS Clang (`-Wreorder-ctor` for ctor
initializer order, unused `#include <climits>` etc.) are noise — the
build won't fail, the warnings just clutter the log. Fix at leisure.

---

## Repo layout

```
.github/workflows/
  build-plugin.yml      # Reusable: takes a plugin name, builds both platforms
  release.yml           # Orchestrator: dispatch entrypoint + tag-based release

scripts/
  assemble-ndk-windows.ps1         # Build Windows portion of NDK tarball
  assemble-ndk-macos_arm64.ps1     # Build macOS portion of NDK tarball

build.py                # Main entrypoint: trigger CI, poll, save binaries

<PluginName>/           # Each plugin self-contained
  CMakeLists.txt
  *.cpp / *.h
  README.md             # Per-plugin user-facing docs
  Release/
    windows/<Plugin>.dll
    macos_arm64/<Plugin>.dylib
```

---

## Releases

Two ways to ship:

1. **Just commit the binaries.** `python build.py <plugin>` → `git commit`
   → `git push`. Team downloads from `github.com/.../<plugin>/Release/`
   directly. This is what we do day-to-day.

2. **Tagged GitHub Release.** Push a tag like `basesixfour-v1.0.0`. The
   workflow picks up the tag, builds, and creates a Release with the
   `.dll` and `.dylib` attached as downloadable assets. The tag prefix
   (lowercased) before `-v` must match the plugin directory name; the
   slug → directory table lives inside `release.yml`'s `parse-tag` job
   and needs updating when a new plugin is added if you intend to use
   tag releases for it.

---

## Rebuilding the NDK tarball

Only needed when Nuke updates (15.2 → 15.3 → 16.x), or if Foundry's
cmake/header layout changes mid-version (rare).

1. On Windows with Nuke installed: `scripts/assemble-ndk-windows.ps1` →
   produces `nuke-ndk-15.2v9/windows/`.
2. Mount the macOS Nuke `.dmg` on Windows (or grab the files from a Mac):
   `scripts/assemble-ndk-macos_arm64.ps1` → produces
   `nuke-ndk-15.2v9/macos_arm64/`.
3. `tar -czf nuke-ndk-15.2v9.tar.gz nuke-ndk-15.2v9/`
4. Upload to S3, replacing the existing object at the same URL.
5. Bump `NDK_VERSION` in `.github/workflows/build-plugin.yml` if the
   directory name inside the tarball changed.

---

## Things that have bitten me

- **MSVC and macOS produce different DLL sizes** for the same source.
  Different toolchain versions, different libwebp build flags, different
  embedded source paths. Both work in Nuke. Don't chase the diff.

- **GitHub Actions reusable-workflow permissions are validated
  statically.** Caller workflows must grant `contents: write` at the
  top-level if any reusable-workflow job declares it, even if that job
  is gated by an `if:` and won't actually run. See top-level
  `permissions:` in `release.yml`.

- **`tar` on the Windows runner is the BSD one shipped with Windows 10+.**
  Handles `-xzf` fine but won't recognize some GNU extensions. Stick to
  plain options.

- **PowerShell scripts saved as UTF-8 without BOM** get misread on
  Windows PowerShell 5.1 (default) — em-dashes etc. become garbage.
  Stick to ASCII in `.ps1` files or save as UTF-8-with-BOM.

- **Nuke's NDK ABI changes between major versions.** A plugin built
  against 15.2 won't load in 16.x. The current setup hard-targets
  Nuke 15.2; if you ever need to support multiple Nuke versions, you
  need a matrix build with separate NDK tarballs and separate output
  dirs.
