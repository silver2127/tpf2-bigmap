# AGENTS.md — guidance for AI coding agents

This is a **native Transport Fever 2 plugin** (`tpf2_bigmap.dll`) for the
`tpf2mp` plugin host that builds maps larger than the New Game menu offers,
plus a companion Lua density mod and a WiX MSI installer.

> Read [README.md](README.md) first — it is a deep, verified technical dossier
> (256 m/tile, the 180-tile street-raster wall, the 32,768 m octree wall, the
> town/industry density math). Everything below is the working summary; the
> README holds the evidence. Do not duplicate it here.

## What is where

| path | purpose |
| --- | --- |
| `src/bigmap.cpp` | the whole plugin — one TU, three independent patch sites |
| `src/tpf2mp_plugin.h` | the **entire** ABI with the host. Vendored; never change its layout incompatibly |
| `cfg/tpf2_bigmap.cfg` | plugin settings. Its comment block is a measurement dossier in its own right |
| `mod/bigmap_density_1/mod.lua` | town/industry density mod (a *mod*, not a `base_config.lua` edit) |
| `tools/test_density.py` | offline lupa-based test of the Lua mod |
| `tools/vendor_host.ps1` | vendor the three shared binaries from a sibling `tpf2-multiplayer` checkout |
| `installer/` | WiX MSI (`Package.wxs` = this product, `PluginHost.wxs` = shared fragment) |
| `build.bat` | standalone MSVC build of the plugin DLL |

## Build & test commands

- Build the plugin: `build.bat` (add `-deploy` to copy into
  `%LOCALAPPDATA%\tpf2mp\data\plugins\`).
  - `LNK1104` = the game is running and holding `tpf2_bigmap.dll`; close it.
- Test the Lua mod offline (no game): `pip install lupa`, then
  `python tools/test_density.py`.
- Vendor shared binaries: `powershell -File tools\vendor_host.ps1 -Build`
  (needs a `tpf2-multiplayer` checkout beside this repo).
- Build the MSI: `powershell -File installer\build_msi.ps1 -Validate -AcceptWixEula`.

## Non-negotiable rules (do not "improve" these)

1. **All RVAs and byte strings are measured on exactly two game binaries** that
   share the same code shape at every hook site: **Steam 35924** (2024-12-11)
   — `0x674aa0` (map size), `0x90d410` (street raster), `0x2304f8` (octree) —
   and **GOG 2024-12-12** — `0x674CC0`, `0x90D500`, `0x230718` (the octree
   `EXPECTED` differs from Steam only in the RIP displacement to its .rdata
   `32768.0f`; `PATCH*` arrays are shared). `Tpf2mpPluginInit` detects which
   one is running by byte-verifying all three GOG sites (`g_gog`), otherwise
   uses the Steam layout via `host->buildOk()`. A build that is neither is
   refused loudly. Do not modernize/hand-tune the constants, and do not add a
   third build without measuring every site on it.
2. **Every patch must byte-verify first.** Pattern: check `host->buildOk()`,
   then `host->verifyBytes(rva, EXPECTED, len)`; on mismatch log loudly and
   **do not patch**. Never weaken that guard. `STEAL`/`STEAL_RASTER` lengths and
   the 13-byte octree rewrite are coupled to the byte arrays — keep them in
   lockstep.
3. **The config section name is load-bearing:** the DLL reads `[tpf2_bigmap]`
   (== `out->name`) from its *own* cfg file, which the host merges over
   `tpf2mp.cfg`.
4. **Do not add `size4_*` / `size5_*` ladder rows.** The detour gets the raw
   combo index and the engine keys the preset table on `sizeIndex` vs
   `sizeIndex+1` depending on `experimentalMapSizes`. The shipped ladder is only
   safe because it claims size 6. There is no guard yet — see the config comment.
5. **Never edit `installer/PluginHost.wxs` or `installer/vendor/*` here.** The
   fragment is byte-identical across MSI repos and carries FIXED component GUIDs
   so Windows Installer reference-counts shared files. Both `build_msi.ps1` and
   `vendor_host.ps1` hash-check it against the `tpf2-multiplayer` copy and fail
   on drift.
6. **A 32-bit multiply widening is NOT a fix** for the street raster. `nx`/`ny`
   are `int32` and every access does `y*nx+x`; the fix is scaling the *cell
   size* (config `street_raster`), which shrinks the counts in range.

## Code conventions

- RVAs are module-relative constants `RVA_<SITE>`; every access adds
  `host->moduleBase()`.
- Prologue byte arrays are `static const uint8_t EXPECTED[...]`; the octree
  replacement is `PATCH_OCTREE`. Trampolines are `g_orig*`, typed via
  `typedef ... (__fastcall *Fn)(...)`.
- `CVec2i` is 8 bytes returned packed in `rax` — the detour returns `uint64_t`.
- Log everything through `host->log("fmt", ...)`; be verbose and conversational
  about what each hook did or refused to do. Partial success still returns
  `TPF2MP_OK` if at least one hook installed.
- Plugin init must validate `abiMajor`/`size` before touching the host, and set
  `out->name = "bigmap"`.

## Lua mod (`mod.lua`) traps

- Read `getCurrentModId()` at **file load**, never inside `runFn` (which runs at
  worldgen, after the loader has moved on to other mods). `runFn` must keep its
  defensive scan of `allModParams` for a key only it declares.
- Multiply `game.config`, don't assign — assignment races mod load order.
- Labels are wrapped in `_()` for translation; log via `print("[bigmap_density] ...")`
  with explicit ACTIVE/INACTIVE states.
- `tools/test_density.py` stubs `_` and `getCurrentModId`; keep that harness green.

## Installer notes

- Version comes from `installer/VERSION` (currently `0.1.0`), validated
  `^\d+\.\d+\.\d+$`.
- `BigmapCfg` component is `NeverOverwrite="yes"` — player ladder edits must
  survive upgrades.
- WiX v7 requires its OSMF EULA; pass `-AcceptWixEula` (the script never
  accepts it for you).
- `tools/test_coexist.ps1` needs elevation and proves install/uninstall order
  independence with real `msiexec` transactions.

## Doc caveat

Prose in `README.md` and the config header uses corrupted path strings
(backslash+letter collapsed into control chars, e.g. `plugins\tpf2_bigmap.cfg`
may read as `plugins<TAB>pf2_bigmap.cfg`). Real paths are correct in code —
never copy a path verbatim from prose; the real names are `tpf2_bigmap.dll`,
`cfg\tpf2_bigmap.cfg`, `mods\bigmap_density_1`, `tools\vendor_host.ps1`,
`installer\build_msi.ps1`. Exception: the `map-size\x04Megalomaniac` `\x04` is
an intentional gettext `msgctxt` separator — leave it alone.
