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

.PARAMETER CMakeArgs
    Extra arguments forwarded verbatim to the configure step. THIS IS HOW THE
    OPTIONAL DEPENDENCIES REACH THE PACKAGED BUILD: this script configures a build
    tree of its own, so flags given to some earlier CMake invocation do not apply
    here. Without them libssh2 and libarchive are simply not found, and the zip
    ships a loftail that cannot open a remote or a compressed log while every test
    in the run passed — the artifact and the tested build are not the same build.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File packaging\windows\build-portable.ps1 `
        -QtDir C:\Qt\6.4.2\msvc2019_64
#>

# loftail — a desktop viewer for log4cplus logs.
# Copyright (C) 2026 Valentyn Pavliuchenko
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

[CmdletBinding()]
param(
    [string]$Config = "Release",
    [string]$QtDir  = "",
    [string[]]$CMakeArgs = @()
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
# NOT named $cmakeArgs: PowerShell variable names are CASE-INSENSITIVE, so that would
# be the same variable as the $CMakeArgs parameter — the assignment below would discard
# the caller's flags and then append the list to itself. It did exactly that once, and
# the only sign was a packaged build quietly missing its optional dependencies.
$configureArgs = @("-S", $RepoRoot, "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=$Config")
if ($QtDir -ne "") { $configureArgs += "-DCMAKE_PREFIX_PATH=$QtDir" }
# Ninja if available, otherwise the default (Visual Studio) generator.
if (Get-Command ninja -ErrorAction SilentlyContinue) { $configureArgs += @("-G", "Ninja") }
if ($CMakeArgs.Count -gt 0) { $configureArgs += $CMakeArgs }
Write-Host ">> cmake $($configureArgs -join ' ')"
cmake @configureArgs 2>&1 | Tee-Object (Join-Path $RepoRoot "configure-portable.log")

# The optional dependencies are what the caller most easily gets wrong here, because
# this is a SEPARATE build tree from whatever was tested. Say what the artifact will
# actually contain rather than leaving it to be discovered by a user.
Select-String -Path (Join-Path $RepoRoot "configure-portable.log") `
              -Pattern "sources: (ENABLED|DISABLED)" |
    ForEach-Object { Write-Host ">> packaged build: $($_.Line.Trim())" }

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

# The licence travels with the binary (GPLv3 §4). The Windows install rule stages the
# bare .exe — there is no share/doc layout to hang it off, unlike the Linux branch — so
# the copy is here, and it must precede the Compress-Archive below.
Copy-Item (Join-Path $RepoRoot "LICENSE") (Join-Path $StageDir "LICENSE.txt")

Write-Host ">> Zipping"
# The release this artifact belongs to, read back from the configured build rather than
# repeated here, so it stays right when project(VERSION) moves. Matches what the .deb and
# the AppImage carry: a file downloaded from a release page has to say which release it
# is once it is out of the browser. It used to carry $Config, which named the same thing
# for every release ever made. $Config survives only when it is not the shipped one, so a
# local Debug zip cannot silently overwrite the Release one beside it in dist\.
$VersionLine = Select-String -Path (Join-Path $BuildDir "CMakeCache.txt") `
                             -Pattern "^CMAKE_PROJECT_VERSION:STATIC=" | Select-Object -First 1
if (-not $VersionLine) {
    throw "could not read CMAKE_PROJECT_VERSION from $BuildDir\CMakeCache.txt"
}
$Version = $VersionLine.Line -replace "^CMAKE_PROJECT_VERSION:STATIC=", ""
# Written as an assignment plus an override rather than `$x = if (…) {…} else {…}`,
# whose line-broken form is the kind of thing only a Windows runner can adjudicate.
$ZipName = "loftail-$Version-windows-x64.zip"
if ($Config -ne "Release") { $ZipName = "loftail-$Version-$Config-windows-x64.zip" }
$Zip = Join-Path $DistDir $ZipName
if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $Zip

Write-Host ">> Built: $Zip"
Get-Item $Zip | Format-List Name, Length
