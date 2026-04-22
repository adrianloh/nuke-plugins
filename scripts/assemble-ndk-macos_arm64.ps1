# assemble-ndk-macos_arm64.ps1
# Collects the macOS arm64 Nuke 15.2v9 NDK bits needed to build plugins in CI
# into ./nuke-ndk-15.2v9/macos_arm64/ in the current directory.
#
# Run this on Windows against a mounted Nuke macOS arm64 .dmg. The .app on the
# dmg appears as a regular directory in Windows Explorer.
#
# On macOS the plugin build only needs headers + cmake config.
# There are NO dylibs to bundle because NukeConfig.cmake uses
# -undefined,dynamic_lookup and defers symbol resolution to Nuke at load time.
#
# If you ever need an Intel build, run this same script against the Intel dmg
# with $DestRoot swapped to macos_x86_64 - the headers/cmake are identical
# across architectures, but keeping them separate lets the CI pick unambiguously.

$ErrorActionPreference = "Stop"

$NukeRoot = "D:\Nuke15.2v9\Nuke15.2v9.app\Contents\MacOS"
$DestRoot = Join-Path (Get-Location) "nuke-ndk-15.2v9\macos_arm64"

# --- Sanity check ----------------------------------------------------

if (-not (Test-Path $NukeRoot)) {
    throw "Nuke mac app not found at $NukeRoot. Is the dmg mounted at D:\?"
}

if (-not (Test-Path (Join-Path $NukeRoot "cmake\NukeConfig.cmake"))) {
    throw "NukeConfig.cmake not found under $NukeRoot\cmake"
}

if (-not (Test-Path (Join-Path $NukeRoot "include\DDImage"))) {
    throw "DDImage headers not found under $NukeRoot\include"
}

Write-Host "Source: $NukeRoot"
Write-Host "Dest:   $DestRoot"
Write-Host ""

# --- Prepare destination ---------------------------------------------

if (Test-Path $DestRoot) {
    Write-Host "Removing existing $DestRoot"
    Remove-Item $DestRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $DestRoot -Force | Out-Null

# --- cmake directory -------------------------------------------------

Write-Host "Copying cmake/ directory..."
Copy-Item (Join-Path $NukeRoot "cmake") -Destination $DestRoot -Recurse
$cmakeCount = (Get-ChildItem (Join-Path $DestRoot "cmake")).Count
Write-Host "  $cmakeCount files"

# --- include directory -----------------------------------------------

Write-Host ""
Write-Host "Copying include/ directory (reading from dmg - can be slow)..."
Copy-Item (Join-Path $NukeRoot "include") -Destination $DestRoot -Recurse
$includeSubdirs = (Get-ChildItem (Join-Path $DestRoot "include") -Directory).Name -join ", "
Write-Host "  subdirs: $includeSubdirs"

# --- Summary ---------------------------------------------------------

Write-Host ""
$totalSize = (Get-ChildItem $DestRoot -Recurse -File | Measure-Object -Property Length -Sum).Sum
$totalMB = [math]::Round($totalSize / 1MB, 1)
$fileCount = (Get-ChildItem $DestRoot -Recurse -File).Count

Write-Host "Done."
Write-Host "  Files:    $fileCount"
Write-Host "  Size:     $totalMB MB"
Write-Host "  Location: $DestRoot"
Write-Host ""
Write-Host "No .dylib files are needed - mac NukeConfig.cmake uses -undefined,dynamic_lookup."
Write-Host "Symbols are resolved by Nuke at plugin load time, not at link time."
Write-Host ""
Write-Host "Next: tar czf nuke-ndk-15.2v9.tar.gz nuke-ndk-15.2v9\ and host it somewhere static."
