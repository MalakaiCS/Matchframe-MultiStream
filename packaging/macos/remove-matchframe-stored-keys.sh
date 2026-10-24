#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# MatchFrame Multistream - delete the plugin's stored keys from the current user's Keychain.
#
# Stream keys, server usernames and passwords are generic-password items with the service name
# "MatchFrame Multistream" in the login Keychain. This deletes every such item; it never prints
# secret values. Usage: sh remove-matchframe-stored-keys.sh [--remove-settings] [--dry-run]
#   --remove-settings  also delete the saved destinations (metadata only) in
#                      ~/Library/Application Support/obs-studio/plugin_config/matchframe-multistream
#   --dry-run          only report what would be deleted
set -eu

service="MatchFrame Multistream"
remove_settings=0
dry_run=0
for arg in "$@"; do
    case "$arg" in
    --remove-settings) remove_settings=1 ;;
    --dry-run) dry_run=1 ;;
    *)
        echo "Usage: sh $0 [--remove-settings] [--dry-run]" >&2
        exit 2
        ;;
    esac
done

# dump-keychain without -d lists attributes only, never the secret data.
count=$(security dump-keychain 2>/dev/null | grep -c "\"svce\"<blob>=\"$service\"" || true)
echo "Stored MatchFrame Multistream entries in the Keychain: $count"

if [ "$dry_run" -eq 1 ]; then
    [ "$remove_settings" -eq 1 ] && echo "Would delete $HOME/Library/Application Support/obs-studio/plugin_config/matchframe-multistream"
    exit 0
fi

deleted=0
# Each call deletes the first matching item; repeat until none is left (bounded for safety).
while [ "$deleted" -lt 1000 ] && security delete-generic-password -s "$service" >/dev/null 2>&1; do
    deleted=$((deleted + 1))
done
echo "Deleted $deleted Keychain entr$([ "$deleted" -eq 1 ] && echo y || echo ies)"

if [ "$remove_settings" -eq 1 ]; then
    settings="$HOME/Library/Application Support/obs-studio/plugin_config/matchframe-multistream"
    if [ -d "$settings" ]; then
        rm -rf "$settings"
        echo "Deleted $settings"
    fi
fi
