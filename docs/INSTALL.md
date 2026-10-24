# Installing MatchFrame Multistream (Windows x64)

MatchFrame Multistream is an OBS Studio plugin. This build targets **OBS Studio 32.2.2 (64-bit) on
Windows 10/11 x64**. Other OBS versions are untested combinations.

## Install

1. Close OBS Studio.
2. Extract the release zip (`matchframe-multistream-<version>-windows-x64.zip`) to a folder.
3. Open **PowerShell as Administrator** in that folder and run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Install-MatchFrameMultistream.ps1
   ```

   The script copies the `matchframe-multistream` folder to
   `%ProgramData%\obs-studio\plugins\matchframe-multistream`, the machine-wide plugin folder that
   OBS 30 and later load automatically. It refuses to run while OBS is open and warns if the installed
   OBS is not 32.2.x.

   Manual alternative: copy the extracted `matchframe-multistream` folder into
   `C:\ProgramData\obs-studio\plugins\` yourself.

4. Start OBS and open **Docks > MatchFrame Multistream**.

The plugin never starts streaming on its own after OBS launches, after a crash, or after a
configuration import. Every output starts only when an operator presses Start.

## Where things are stored

| What | Where |
| --- | --- |
| Plugin binaries | `%ProgramData%\obs-studio\plugins\matchframe-multistream\` |
| Destinations and shared encoding settings (no secrets) | `%APPDATA%\obs-studio\plugin_config\matchframe-multistream\profiles\<OBS profile>.json` (+ `.bak`) |
| Stream keys, server usernames and passwords | Windows Credential Manager (Windows Credentials > Generic), targets `MatchFrameMultistream/v1/<profile id>/<destination id>/<field>`, for the Windows user running OBS |

If Windows Credential Manager refuses a write, the editor offers to keep that key in memory for the
current OBS session only. Keys are never written to plain-text files.

## Uninstall

1. Close OBS Studio.
2. In an **elevated** PowerShell in the extracted release folder:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Uninstall-MatchFrameMultistream.ps1
   ```

3. To also delete the stored keys and saved destinations of a Windows user, sign in as that user and
   run, in a **normal (not elevated)** PowerShell:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Remove-MatchFrameStoredKeys.ps1 -RemoveSettings
   ```

   Add `-WhatIf` first to list what would be deleted. Without `-RemoveSettings` only the keys are
   removed. Credential Manager entries are per Windows user, which is why this step is separate from
   the elevated uninstall.

## Upgrading

Run the install script from the new release; it replaces the previous binaries. Saved destinations
and stored keys are kept.
