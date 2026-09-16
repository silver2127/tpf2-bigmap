#!/usr/bin/env bash
set -euo pipefail
root=${XDG_DATA_HOME:-$HOME/.local/share}/tpf2mp
if [ -z "${XDG_DATA_HOME:-}" ] && [ ! -e "$root/tpf2_pluginhost.so" ] &&
   [ -d "$HOME/snap/steam/common/.local/share/Steam/steamapps" ]; then
  root=$HOME/snap/steam/common/.local/share/tpf2mp
fi
if [ "${1:-}" = --prefix ]; then root=${2:?}; shift 2; fi
[ $# = 0 ] || { echo "Usage: $0 [--prefix DIRECTORY]" >&2; exit 2; }
if [ -f "$root/data/bigmap-base-mod.path" ]; then
  IFS= read -r game_base_mod < "$root/data/bigmap-base-mod.path"
  "$root/bigmap-density-restore" "$game_base_mod"
  rm -f "$root/data/bigmap-base-mod.path"
fi
rm -f "$root/bigmap-density-restore"
rm -f "$root/data/plugins/tpf2_bigmap.so" "$root/tpf2-bigmap-launch"
echo 'Removed the big-map plugin. Kept your config, saves and the shared plugin host.'
echo 'If you used tpf2-bigmap-launch in Steam launch options, remove that entry.'
