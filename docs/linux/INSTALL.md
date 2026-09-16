# Native Linux big maps (experimental)

For Steam Transport Fever 2 Linux build 35924. This is a native `.so` plugin,
not a Wine/Proton DLL. The package includes the shared native plugin host;
multiplayer is not required.

1. Close the game and extract `tpf2-bigmap-0.4.0-linux-dev.2.tar.gz`.
2. In the extracted directory, run `bash install.sh` without sudo.
3. Use the Steam launch-options line printed by the installer. If multiplayer
   is installed, keep its existing launch options: it loads this plugin too.
4. Open **New Game**. Nine extra size rows extend the menu through
   **130.56 x 130.56 km** (510 x 510 tiles); ratios extend through **1:20**.

The defaults are installed under
`${XDG_DATA_HOME:-$HOME/.local/share}/tpf2mp/data/plugins/tpf2_bigmap.cfg`.
`--prefix /absolute/path/to/tpf2mp` selects a different shared installation.
Snap Steam is detected when XDG_DATA_HOME is unset and no default host exists;
its prefix is `~/snap/steam/common/.local/share/tpf2mp`. Restart
for configuration changes. Existing configs are kept on upgrade; updated
Linux defaults are placed in `.cfg.example`.

Use the plugin when loading worlds that need its expanded octree. Back up
important saves before testing. Larger maps need substantially more RAM and
generation time. This port does not include Windows terrain/material paging.

## Supported scope

- Added size rows, stock presets, extended ratios and explicit size cells.
- Six sparse density presets in Towns, Industries and Industry density target.
- Adaptive street occupancy cells to avoid signed 32-bit overflow.
- Depth-11 octree with a 512-tile edge cap and unchanged 128 m leaves.
  The largest square is 510 tiles; the preview distance calculation imposes
  a separate diagonal bound until its Linux overflow fix is ported.
- Byte/build guards and a shared host that coexists with native multiplayer.

Depth 12/13 (1024/2048-tile edges), Windows fault-driven RAM compression,
the Windows engine speed optimizations are
not ported in this build. Do not use the Windows configuration: its depth-13
setting is rejected explicitly. See PORT.md for evidence and validation limits.

## Sparse density presets

After **Very high**, all three density dropdowns offer Reduced (0.50), Sparse
(0.30), Megalomaniac count at 56 km (0.18), Minimal (0.10), and Megalomaniac
count at 112 km (0.046) / 160 km (0.022). Values are fractions of **Medium**
density, not fixed object counts. Map area and generator constraints affect
actual counts. Stock Low through Very high keep their original values.

The feature is on by default (`newgame_density=1`, including upgrades with an
older config). Restart the game after installing. Select Towns and Industries
on the first New Game page; the later Industry density target controls future
industry spawning. Existing towns/industries are not removed by these settings.

The plugin patches the installed game's `res/config/base_mod.lua` and keeps
`base_mod.lua.bigmap-linux.bak`. Missing or changed anchors stop initialization;
no extra labels are exposed without the matching native town hook. Steam file
verification is handled by patching the restored stock file on the next launch.
The uninstaller restores the backup only when the patched file is unchanged;
it preserves later manual edits and reports a failure instead of deleting them.

Keep density support enabled when loading saves made with the extra industry
presets: their stored indices need the added Lua multipliers. The big-map plugin
must also be present on other machines loading those saves.

## Remove

Run `bash uninstall.sh` (with the same `--prefix` if supplied). It removes only
the big-map plugin and its standalone launcher. Config, saves, multiplayer and
the shared plugin host remain. Remove `tpf2-bigmap-launch` from Steam options
if you used it; an existing multiplayer launch line should stay.

Logs: `<prefix>/data/tpf2mp_host.log`. Look for `tpf2_bigmap ... -> OK (0)` and
`size dropdown: ... stock + 9 native Linux rows`.
