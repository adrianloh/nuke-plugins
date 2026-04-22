# nuke-plugins

A collection of Foundry Nuke NDK plugins built for Windows x86_64 and macOS
arm64, Nuke 15.2.x.

## Plugins

| Name          | Purpose                                                   |
| ------------- | --------------------------------------------------------- |
| BaseSixFour   | Encode input image to WebP/JPEG/PNG and output as base64  |

_More to come. Each plugin lives in its own top-level directory and is
independently buildable — you can extract any of them into a standalone repo
with no changes._

## Building

### Locally

Each plugin is a standalone CMake project. From inside the plugin directory:

```bash
# Windows
cmake -S . -B build -DNUKE_ROOT="C:/Program Files/Nuke15.2v9" -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# macOS
cmake -S . -B build -DNUKE_ROOT=/Applications/Nuke15.2v9/Nuke15.2v9.app/Contents/MacOS -G Ninja
cmake --build build --config Release
```

### In CI (recommended)

The repo has two workflows:

- **`.github/workflows/release.yml`** — orchestrator. Two triggers:
  1. **Manual dispatch** — go to Actions tab, pick "Build / Release", choose a
     plugin from the dropdown, click Run. Builds both platforms, uploads
     artifacts.
  2. **Git tag** — pushing a tag like `basesixfour-v1.0.0` builds the matching
     plugin on both platforms and publishes a GitHub Release with both
     binaries attached.

- **`.github/workflows/build-plugin.yml`** — reusable workflow that does the
  actual build. Called by `release.yml`; not invoked directly.

Both workflows pull the Nuke NDK (headers + CMake config only, ~3 MB) from
an S3 tarball at build time.

## Tagging convention

Tags follow `<plugin-slug>-v<semver>`:

- `basesixfour-v1.0.0`
- `loki-v2.3.1`
- `webpreader-v0.1.0`

The slug is lowercase; the orchestrator maps it back to the real directory
name via a table in `release.yml`.

## Adding a new plugin

1. Create a new top-level directory with its own `CMakeLists.txt`. Use
   `BaseSixFour/CMakeLists.txt` as a template — the boilerplate is
   `find_package(Nuke REQUIRED)` + `add_nuke_plugin(<Name> <sources>)`.
2. Edit `.github/workflows/release.yml`:
   - Add the plugin name to the `choice` dropdown under
     `workflow_dispatch.inputs.plugin.options`.
   - Add a case to the slug → directory table inside `parse-tag`.
3. Commit. Push. Your new plugin is now buildable via dispatch or by tagging.

## Repo layout

```
.github/workflows/
  build-plugin.yml      # Reusable: takes a plugin name, builds both platforms
  release.yml           # Orchestrator: dispatch dropdown + tag router

scripts/
  assemble-ndk-windows_x86_64.ps1  # Pull Windows NDK bits from a Nuke install
  assemble-ndk-macos_arm64.ps1     # Pull macOS NDK bits from a mounted .dmg

BaseSixFour/
  CMakeLists.txt
  BaseSixFour.cpp
  stb_impl.cpp
  stb_image_write.h
```

## Nuke NDK tarball

The NDK tarball at `https://smack.s3.ap-southeast-1.amazonaws.com/nuke-ndk-15.2v9.tar.gz`
contains only what's needed to link plugins: headers and CMake config files.
It does **not** contain any Nuke binaries. To rebuild the tarball from a
licensed Nuke install (if Nuke is updated):

1. Run `scripts/assemble-ndk-windows_x86_64.ps1` on a Windows machine with
   Nuke installed.
2. Run `scripts/assemble-ndk-macos_arm64.ps1` on Windows with a mounted mac
   Nuke `.dmg` (or adapt to run on macOS directly).
3. `tar -czf nuke-ndk-15.2v9.tar.gz nuke-ndk-15.2v9/` and re-upload to S3.

## License

Per-plugin. See each plugin's directory for details.
