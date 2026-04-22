# assemble-ndk-windows.ps1
# Collects the Windows Nuke 15.2v9 NDK bits needed to build plugins in CI
# into ./nuke-ndk-15.2v9/windows/ in the current directory.

$ErrorActionPreference = "Stop"

$NukeRoot = "C:\Program Files\Nuke15.2v9"
$DestRoot = Join-Path (Get-Location) "nuke-ndk-15.2v9\windows"

# --- Sanity check ----------------------------------------------------

if (-not (Test-Path $NukeRoot)) {
    throw "Nuke install not found at $NukeRoot"
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

# --- .lib files ------------------------------------------------------

$Libs = @(
    "DDImage.lib",
    "FdkBase.lib",
    "FnUsdAbstraction.lib",
    "FnUsdEngine.lib",
    "Ndk.lib",
    "RIPFramework.lib",
    "glew32.lib",
    "tbb.lib",
    "tbbmalloc.lib"
)

Write-Host "Copying .lib files..."
foreach ($lib in $Libs) {
    $src = Join-Path $NukeRoot $lib
    if (-not (Test-Path $src)) {
        throw "Missing required library: $src"
    }
    Copy-Item $src -Destination $DestRoot
    Write-Host "  $lib"
}

# --- cmake directory -------------------------------------------------

Write-Host ""
Write-Host "Copying cmake/ directory..."
Copy-Item (Join-Path $NukeRoot "cmake") -Destination $DestRoot -Recurse
$cmakeCount = (Get-ChildItem (Join-Path $DestRoot "cmake")).Count
Write-Host "  $cmakeCount files"

# --- include directory -----------------------------------------------

Write-Host ""
Write-Host "Copying include/ directory (this takes a moment)..."
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
Write-Host "Next: grab the macOS side into nuke-ndk-15.2v9\macos\, then tar czf the whole nuke-ndk-15.2v9\ directory."
