#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# MatchFrame Multistream - macOS uninstaller.
#
# Removes the plugin that the installer package put in the current user's OBS plugin folder.
# Saved destinations and stored stream keys are kept; run remove-matchframe-stored-keys.sh
# for those. Usage: sh uninstall-matchframe-multistream.sh
set -eu

if pgrep -x OBS >/dev/null 2>&1; then
    echo "OBS Studio is running. Quit OBS before uninstalling the plugin." >&2
    exit 1
fi

plugins="$HOME/Library/Application Support/obs-studio/plugins"
removed=0
for item in "$plugins/matchframe-multistream.plugin" "$plugins/matchframe-multistream.plugin.dSYM"; do
    if [ -e "$item" ]; then
        rm -rf "$item"
        echo "Removed $item"
        removed=1
    fi
done
[ "$removed" -eq 1 ] || echo "Nothing to remove in $plugins"

# The package was installed into the home folder, so its receipt is registered there.
pkgutil --volume "$HOME" --forget com.matchframe.multistream >/dev/null 2>&1 || true

echo "To also delete stored stream keys and saved destinations, run:"
echo "  sh remove-matchframe-stored-keys.sh --remove-settings"
