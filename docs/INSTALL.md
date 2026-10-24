# Installing MatchFrame Multistream

MatchFrame Multistream is an OBS Studio plugin built for **OBS Studio 32.2.2**. Other OBS versions
are untested combinations. Windows x64 is the primary platform; a macOS package (Apple silicon
and Intel, macOS 13 or later) is also provided — see [macOS](#macos) below.

## Windows x64

### Install

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

### Where things are stored

| What | Where |
| --- | --- |
| Plugin binaries | `%ProgramData%\obs-studio\plugins\matchframe-multistream\` |
| Destinations and shared encoding settings (no secrets) | `%APPDATA%\obs-studio\plugin_config\matchframe-multistream\profiles\<OBS profile>.json` (+ `.bak`) |
| Stream keys, server usernames and passwords | Windows Credential Manager (Windows Credentials > Generic), targets `MatchFrameMultistream/v1/<profile id>/<destination id>/<field>`, for the Windows user running OBS |

If Windows Credential Manager refuses a write, the editor offers to keep that key in memory for the
current OBS session only. Keys are never written to plain-text files.

### Uninstall

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

### Upgrading

Run the install script from the new release; it replaces the previous binaries. Saved destinations
and stored keys are kept.

## macOS

The macOS build is a universal binary (Apple silicon and Intel) for **OBS Studio 32.2.2 on macOS 13
Ventura or later**.

### Install

1. Quit OBS Studio.
2. Open `matchframe-multistream-<version>-macos-universal.pkg` (also inside the macOS release zip)
   and follow the installer. It installs `matchframe-multistream.plugin` for the current user into
   `~/Library/Application Support/obs-studio/plugins/`, the folder OBS 32 loads plugins from.
   No administrator password is needed.

   The package is **not signed or notarized** (that needs an Apple Developer ID, which this
   project does not have yet). macOS therefore refuses to open it with a double-click. Control-click
   the package, choose **Open**, then confirm — or allow it under **System Settings > Privacy &
   Security** after the first attempt. From Terminal the same package installs with:

   ```sh
   installer -pkg matchframe-multistream-<version>-macos-universal.pkg -target CurrentUserHomeDirectory
   ```

3. Start OBS and open **Docks > MatchFrame Multistream**.

### Where things are stored

| What | Where |
| --- | --- |
| Plugin | `~/Library/Application Support/obs-studio/plugins/matchframe-multistream.plugin` |
| Destinations and shared encoding settings (no secrets) | `~/Library/Application Support/obs-studio/plugin_config/matchframe-multistream/profiles/<OBS profile>.json` (+ `.bak`) |
| Stream keys, server usernames and passwords | The login Keychain (Keychain Access > login > Passwords), items named "MatchFrame Multistream stream credential", service `MatchFrame Multistream`, account `MatchFrameMultistream/v1/<profile id>/<destination id>/<field>` |

If the Keychain refuses a write (for example while it is locked), the editor offers to keep that
key in memory for the current OBS session only. Keys are never written to plain-text files.

### Uninstall

From the folder of the extracted macOS release zip, in Terminal:

```sh
sh uninstall-matchframe-multistream.sh                         # removes the plugin
sh remove-matchframe-stored-keys.sh --remove-settings          # also deletes stored keys and saved destinations
```

`remove-matchframe-stored-keys.sh --dry-run` reports what would be deleted. Without
`--remove-settings` only the Keychain entries are removed.
