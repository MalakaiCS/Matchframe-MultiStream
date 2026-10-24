# SPDX-License-Identifier: GPL-2.0-or-later
# MatchFrame Multistream - Windows uninstaller script.
#
# Removes the plugin from OBS Studio's machine-wide plugin folder. Saved destinations
# (per-user OBS plugin_config) and stored stream keys (per-user Windows Credential Manager)
# are NOT removed by this elevated script; run Remove-MatchFrameStoredKeys.ps1 as the OBS
# user (not elevated) for that.
#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [string] $PluginsDirectory = (Join-Path $env:ProgramData 'obs-studio\plugins')
)

$ErrorActionPreference = 'Stop'

if (Get-Process -Name 'obs64' -ErrorAction SilentlyContinue) {
    throw 'OBS Studio is running. Close OBS before uninstalling the plugin.'
}

$Destination = Join-Path $PluginsDirectory 'matchframe-multistream'
if (Test-Path $Destination) {
    Remove-Item -Recurse -Force $Destination
    Write-Host "Removed $Destination"
} else {
    Write-Host "Nothing to remove at $Destination"
}
Write-Host 'To also delete stored stream keys and saved destinations for a Windows user, run'
Write-Host '  powershell -ExecutionPolicy Bypass -File .\Remove-MatchFrameStoredKeys.ps1 -RemoveSettings'
Write-Host 'in a normal (not elevated) PowerShell signed in as that user.'
