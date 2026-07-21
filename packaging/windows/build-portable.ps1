<#
.SYNOPSIS
    Build a self-contained portable loftail zip on Windows.

.DESCRIPTION
    Configures + builds loftail (Release), installs the bare executable, runs
    windeployqt to copy the Qt runtime (DLLs, platform plugins, styles) next to
    it, and zips the result. The zip runs on a machine with no Qt installed.

    An MSI/NSIS installer is an option on top of this (it would add Start-menu
    shortcuts and, if desired, .log file association — which the application
    itself never registers, per PLAN.md M7). The portable zip is the baseline
    artifact and needs no installer.

    STATUS: authored on Linux, NOT yet run/verified on Windows. Run this on a
    Windows machine with Qt 6 (>= 6.4) and a C++ toolchain (MSVC or MinGW) on
    PATH, then clean-machine-verify per packaging/README.md.

.PARAMETER Config
    CMake build type. Default: Release.

.PARAMETER QtDir
    Qt prefix (the dir containing bin/windeployqt.exe). If omitted, windeployqt
    is expected on PATH and CMAKE_PREFIX_PATH is assumed already set.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File packaging\windows\build-portable.ps1 `
        -QtDir C:\Qt\6.4.2\msvc2019_64
#>
[CmdletBinding()]
param(
    [string]$Config = "Release",
    [string]$QtDir  = ""
)

$ErrorActionPreference = "Stop"

# Repo root = two levels up from this script (packaging\windows\).
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
Set-Location $RepoRoot

$BuildDir   = Join-Path $RepoRoot "build-windows"
$StageDir   = Join-Path $RepoRoot "dist\loftail-portable"
$DistDir    = Join-Path $RepoRoot "dist"

# Locate windeployqt.
if ($QtDir -ne "") {
    $env:CMAKE_PREFIX_PATH = $QtDir
    $WinDeployQt = Join-Path $QtDir "bin\windeployqt.exe"
} else {
    $WinDeployQt = "windeployqt.exe"
}

Write-Host ">> Configuring ($Config)"
$cmakeArgs = @("-S", $RepoRoot, "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=$Config")
if ($QtDir -ne "") { $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtDir" }
# Ninja if available, otherwise the default (Visual Studio) generator.
if (Get-Command ninja -ErrorAction SilentlyContinue) { $cmakeArgs += @("-G", "Ninja") }
cmake @cmakeArgs

Write-Host ">> Building"
cmake --build $BuildDir --config $Config

Write-Host ">> Staging into $StageDir"
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
cmake --install $BuildDir --config $Config --prefix $StageDir

$Exe = Join-Path $StageDir "loftail.exe"
if (-not (Test-Path $Exe)) {
    # Multi-config generators may nest the exe; fall back to a search.
    $found = Get-ChildItem -Recurse -Filter "loftail.exe" $BuildDir | Select-Object -First 1
    if ($found) { Copy-Item $found.FullName $Exe }
}

Write-Host ">> Running windeployqt"
& $WinDeployQt --release --no-translations --compiler-runtime $Exe

Write-Host ">> Zipping"
$Zip = Join-Path $DistDir "loftail-$Config-windows-x64.zip"
if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $Zip

Write-Host ">> Built: $Zip"
Get-Item $Zip | Format-List Name, Length
