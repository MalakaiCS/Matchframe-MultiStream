# SPDX-License-Identifier: GPL-2.0-or-later
# MatchFrame Multistream - removes this plugin's stream keys and server credentials from the
# CURRENT user's Windows Credential Manager (targets "MatchFrameMultistream/v1/*").
# Run as the Windows user who runs OBS, in a normal (not elevated) PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\Remove-MatchFrameStoredKeys.ps1 [-RemoveSettings] [-WhatIf]
# -RemoveSettings also deletes the plugin's saved destinations (metadata only) from
# %APPDATA%\obs-studio\plugin_config\matchframe-multistream.

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch] $RemoveSettings
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class MatchFrameCredentials {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct CREDENTIAL {
        public uint Flags;
        public uint Type;
        public IntPtr TargetName;
        public IntPtr Comment;
        public long LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        public IntPtr TargetAlias;
        public IntPtr UserName;
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredEnumerateW(string filter, uint flags, out uint count, out IntPtr credentials);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool CredDeleteW(string target, uint type, uint flags);

    [DllImport("advapi32.dll")]
    private static extern void CredFree(IntPtr buffer);

    public const uint CRED_TYPE_GENERIC = 1;

    public static string[] Targets(string filter) {
        uint count;
        IntPtr list;
        var targets = new List<string>();
        if (!CredEnumerateW(filter, 0, out count, out list))
            return targets.ToArray(); // ERROR_NOT_FOUND: nothing stored
        try {
            for (int i = 0; i < count; i++) {
                IntPtr item = Marshal.ReadIntPtr(list, i * IntPtr.Size);
                var credential = (CREDENTIAL)Marshal.PtrToStructure(item, typeof(CREDENTIAL));
                if (credential.Type == CRED_TYPE_GENERIC)
                    targets.Add(Marshal.PtrToStringUni(credential.TargetName));
            }
        } finally {
            CredFree(list);
        }
        return targets.ToArray();
    }
}
'@

$Prefix = 'MatchFrameMultistream/v1/'
$Targets = [MatchFrameCredentials]::Targets("$Prefix*")
if ($Targets.Count -eq 0) {
    Write-Host 'No MatchFrame Multistream credentials are stored for this Windows user.'
}
foreach ($Target in $Targets) {
    if ($PSCmdlet.ShouldProcess($Target, 'Delete credential')) {
        if ([MatchFrameCredentials]::CredDeleteW($Target, [MatchFrameCredentials]::CRED_TYPE_GENERIC, 0)) {
            Write-Host "Deleted $Target"
        } else {
            Write-Warning "Could not delete $Target (error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
        }
    }
}

if ($RemoveSettings) {
    $Settings = Join-Path $env:APPDATA 'obs-studio\plugin_config\matchframe-multistream'
    if ((Test-Path $Settings) -and $PSCmdlet.ShouldProcess($Settings, 'Delete saved destinations')) {
        Remove-Item -Recurse -Force $Settings
        Write-Host "Deleted $Settings"
    }
}
