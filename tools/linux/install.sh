#!/usr/bin/env bash
set -euo pipefail
package=$(cd "$(dirname "$0")" && pwd)
root=${XDG_DATA_HOME:-$HOME/.local/share}/tpf2mp
if [ -z "${XDG_DATA_HOME:-}" ] && [ ! -e "$root/tpf2_pluginhost.so" ] &&
   [ -d "$HOME/snap/steam/common/.local/share/Steam/steamapps" ]; then
  root=$HOME/snap/steam/common/.local/share/tpf2mp
fi
while [ $# -gt 0 ]; do
  case "$1" in --prefix) root=${2:?}; shift 2;; *) echo "Usage: $0 [--prefix DIRECTORY]" >&2; exit 2;; esac
done
case "$root" in *[[:space:]:]*) echo 'Install path must not contain whitespace or colon (LD_PRELOAD).' >&2; exit 1;; esac
(cd "$package" && sha256sum -c SHA256SUMS)
mkdir -p "$root/data/plugins"
# Rename into place: never overwrite an inode a running loader may have mapped.
install -m 0755 "$package/plugins/tpf2_bigmap.so" "$root/data/plugins/tpf2_bigmap.so.new"
mv -f "$root/data/plugins/tpf2_bigmap.so.new" "$root/data/plugins/tpf2_bigmap.so"
if [ ! -e "$root/data/plugins/tpf2_bigmap.cfg" ]; then
  install -m 0644 "$package/plugins/tpf2_bigmap.cfg" "$root/data/plugins/tpf2_bigmap.cfg"
else
  install -m 0644 "$package/plugins/tpf2_bigmap.cfg" "$root/data/plugins/tpf2_bigmap.cfg.example"
  echo 'Kept your existing config; the Linux defaults are in tpf2_bigmap.cfg.example.'
fi
# The multiplayer installer owns this shared host when already installed.
if [ ! -e "$root/tpf2_pluginhost.so" ]; then
  install -m 0755 "$package/runtime/tpf2_pluginhost.so" "$root/tpf2_pluginhost.so.new"
  mv "$root/tpf2_pluginhost.so.new" "$root/tpf2_pluginhost.so"
fi
install -m 0755 "$package/bigmap-density-restore" "$root/bigmap-density-restore"
install -m 0755 "$package/tpf2-bigmap-launch" "$root/tpf2-bigmap-launch"
if [ -x "$root/tpf2mp-launch" ]; then
  echo "Installed. Keep your existing multiplayer Steam launch options."
else
  printf 'Installed. Steam launch options:\n"%s/tpf2-bigmap-launch" %%command%%\n' "$root"
fi
printf 'Config: %s/data/plugins/tpf2_bigmap.cfg\nRestart the game to load the plugin.\n' "$root"
