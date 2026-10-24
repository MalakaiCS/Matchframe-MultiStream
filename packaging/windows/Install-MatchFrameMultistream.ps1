# SPDX-License-Identifier: GPL-2.0-or-later
# MatchFrame Multistream - Windows installer script.
#
# Copies the plugin into OBS Studio's machine-wide plugin folder
# (%ProgramData%\obs-studio\plugins), the location OBS 30+ scans for third-party plugins.
# Run from the extracted release folder in an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\Install-MatchFrameMultistream.ps1
#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [string] $PluginsDirectory = (Join-Path $env:ProgramData 'obs-studio\plugins')
)

$ErrorActionPreference = 'Stop'
$PluginName = 'matchframe-multistream'
$Source = Join-Path $PSScriptRoot $PluginName

if (-not (Test-Path (Join-Path $Source 'bin\64bit\matchframe-multistream.dll'))) {
    throw "Plugin files not found next to this script ($Source). Extract the whole release zip first."
}

if (Get-Process -Name 'obs64' -ErrorAction SilentlyContinue) {
    throw 'OBS Studio is running. Close OBS before installing or updating the plugin.'
}

$ObsExe = Join-Path $env:ProgramFiles 'obs-studio\bin\64bit\obs64.exe'
if (Test-Path $ObsExe) {
    $Version = (Get-Item $ObsExe).VersionInfo.ProductVersion
    Write-Host "Found OBS Studio $Version"
    if ($Version -notlike '32.2*') {
        Write-Warning "This build was compiled and tested against OBS Studio 32.2.2. OBS $Version is not a tested combination."
    }
} else {
    Write-Warning "OBS Studio was not found at $ObsExe. Installing anyway."
}

$Destination = Join-Path $PluginsDirectory $PluginName
New-Item -ItemType Directory -Force -Path $PluginsDirectory | Out-Null
if (Test-Path $Destination) {
    Write-Host "Replacing existing installation at $Destination"
    Remove-Item -Recurse -Force $Destination
}
Copy-Item -Recurse -Path $Source -Destination $Destination
Write-Host "Installed MatchFrame Multistream to $Destination"
Write-Host 'Start OBS and open Docks > MatchFrame Multistream.'
