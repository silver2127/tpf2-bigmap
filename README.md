# tpf2-bigmap

Maps larger than Transport Fever 2's New Game menu will build.

A native plugin for the **tpf2mp plugin host**. It carries no multiplayer code
and has no build-time dependency on the host tree — only the vendored
`src/tpf2mp_plugin.h`, which is the whole ABI.

Target: **Transport Fever 2 build 35924** (2024-12-11, the last release). Every
address here was measured on that build, and the plugin refuses to patch
anything else.

---

## What the game does

The New Game menu turns two dropdown indices into a tile count:

```
CVec2i UI::`anonymous-namespace'::GetNumTilesNew(int sizeIndex,
                                                 int formatIndex,
                                                 const AppConfig&)
RVA 0x674aa0   MenuUI.cpp:264-273
```

and the caller expands that straight into a heightmap:

```
dim         = 1 << terrainLevels        // 64
heightmapPx = tiles * dim + 1
```

**One tile is 256 m** (64 px at 4.0 m/px). Measured directly: a 224 x 224 map
reports a world bounding box of **57,344 m** per side, and 57344 / 224 = 256
exactly. 4.0 m/px is also the round number you would expect a terrain LOD scheme
to use; 3.90625 is not.

Every shipped preset, at 256 m/tile:

| tiles | km | preset |
| --- | --- | --- |
| 18 x 54 | 4.6 x 13.8 | Small 1:3 |
| 22 x 88 | 5.6 x 22.5 | Medium 1:4 |
| 48 x 192 | 12.3 x 49.2 | Megalomaniac 1:4 |
| 54 x 162 | 13.8 x 41.5 | Megalomaniac 1:3 |
| 66 x 132 | 16.9 x 33.8 | Megalomaniac 1:2 |
| 96 x 96 | 24.6 x 24.6 | Megalomaniac 1:1 |

The tile COUNTS come from the `.sav` header (`numTilesX`/`numTilesY` at
+0x10/+0x14 after zstd decompression) and are certain. The km column is derived
from them -- so it is not independent evidence for the tile size, which is why
the bounding box above is what settles it.

Read them yourself:

```python
import zstandard as zstd, struct
head = zstd.ZstdDecompressor().stream_reader(open(sav, 'rb')).read(64)
assert head[:4] == b'tf**'
print(struct.unpack_from('<8i', head, 4)[3:5])   # numTilesX, numTilesY
```

## The two ceilings

`GetNumTilesNew` clamps both axes to 224 tiles:

```
0x674afa:  B9 E0 00 00 00     mov ecx, 0xE0        ; 224
           cmp edx, ecx / cmovle ...               ; applied to x and y
```

So the presets are not the real limit. Two separate ways past them:

**Without this plugin at all.** `settings.lua` has a shipped, undocumented
escape hatch:

```lua
newGameMenuState = {
    worldDimensionsOverride = { 224, 224 },   -- 56 x 56 km
```

Read at `AppConfig+0x28/+0x2c`; if both components are positive it bypasses the
preset table entirely. Both must be **even** (there is a live assert) and it is
still subject to the 224 clamp. Edit it with the game **closed** — the game
rewrites `settings.lua` on exit.

That alone gets you 56 × 56 km = 3,136 km², against the 576 km² every
Megalomaniac variant is capped at. The presets conserve area as you stretch the
ratio; the override does not.

**With this plugin**, past the clamp — because a detour never reaches it.

## The 180-tile wall, and how the plugin gets past it

The game's own 224-tile clamp is not reachable with stock code. "Creating
streets" allocates a `std::vector<bool>` with **one bit per square metre** over
the whole map bounding box, and sizes it with a 32-bit multiply (RVA `0x90d410`):

```
mov    eax, [rbx+0x44]          ; ny
imul   eax, dword [rbx+0x40]    ; nx * ny   <-- 32-bit signed
movsxd rdx, eax                 ; sign-extend into size_t
call   vector<bool>::resize
```

At 224 tiles the map is 57,344 m per side, so `57,345² = 3,288,449,025 > INT_MAX`.
It wraps negative, sign-extends to ~1.8e19, and `resize` throws
`std::length_error`. Nothing catches it: `terminate` → `abort` → SIGABRT with no
message, because it is an **uncaught C++ exception, not an assert**. Confirmed by
resolving the thrown object's RTTI in the minidump (`.?AVlength_error@std@@`).

Stock ceiling: `(width_m + 1) × (height_m + 1) ≤ 2,147,483,647`, i.e. width_m ≤
46,340, i.e. **180 tiles (46.1 km)** on a square map -- tiles must be even, and
182 (46,592 m) already overflows.

### Why we do not just widen the multiply

Because it would make things worse. `nx` and `ny` are stored as `int32` at
`+0x40`/`+0x44` and every access computes an index like `y*nx + x`. A correctly
sized vector would still be addressed with wrapped negative indices past 2³¹
cells — **silent memory corruption instead of a clean abort**. Fixing it properly
means auditing every indexing site in the streets pass, and one missed site has
no symptom.

### What we do instead: scale the cell size (`street_raster=1`)

`cellSize` is an *argument* (`xmm2`), so the plugin detours the constructor and
grows it until the cell count fits a budget. `nx` and `ny` shrink, so every
downstream int32 index stays in range untouched — no audit, no corruption risk.

| map | cell | cells | vs INT_MAX | raster |
| --- | --- | --- | --- | --- |
| 24.6 km (stock) | 1 m | 0.60e9 | 28% | 75 MB — **untouched** |
| 57.3 km | 2 m | 0.82e9 | 38% | 103 MB |
| 114.7 km | 3 m | 1.46e9 | 68% | 183 MB |

Below the budget it is a no-op, so normal maps keep their 1 m grid and behave
exactly as before. The cost above it is road-placement granularity — 2 m instead
of 1 m, against roads 10–20 m wide.

### The 32,768 m wall (the real ceiling, for now)

A 320-tile map (±40,960 m) generates fine and then corrupts itself during play.
The street builder creates **duplicate base nodes** — 2 to 5 nodes at one
position — and every town-development or industry-connect step that touches one
fails down the same chain:

```
Duplicate base nodes found at world position: (36918 / 17379 / 6.75) with entitiy IDs ...
 Trying to merge duplicate nodes with 2005741
 Base node deduplication failed.
 Merging did not succeed, trying to delete now.
 Trying to delete duplicate node 2005741
transition_util.cpp:41 GetNodeShapeAttributes: Assertion `ctx.size() >= 2' failed
```

168 times in half an hour. Towns in the affected band never get streets, so they
generate with **zero population**; terrain LOD is visibly wrong in the same band.

The positions are the tell. All 21 distinct duplicate positions have
`max(|x|,|y|)` between **34,175 and 40,082 m — not one inside 32,768 m** — spread
evenly over all four edges. A 224-tile map (±28,672 m) shows none of it.

**32,768 = 2¹⁵ = 256 tiles ÷ 2 — and it is `ecs::OctreeSystem`'s root box.** Every
street node, construction and vehicle lives in that octree, and its root is a
two-tier *constant*, never derived from the map:

```
0x1402304de  cmp dword [r14], 0x80        ; numTilesX > 128 ?
0x1402304ee  cmp dword [r14+4], 0x80      ; numTilesY > 128 ?
0x1402304f8  movss xmm2, [32768.0f]       ; > 128 tiles: ±32,768 m
0x140230500  mov   edx, 0xa               ;              depth 10
0x140230509  call  Octree::Resize
```

(≤ 128 tiles gets ±16,384 m / depth 9. There is no third branch.) Inserts never
test containment, so an entity past the box just walks to the boundary leaf.
Every query prunes on node boxes *first*, so anything beyond ~32,832 m is
invisible to lookups — and the street builder, finding nothing there, stacks a
new node on top of the old one. That is the whole bug.

The `Duplicate base nodes found` lines are printed by a *load-time* repair pass,
not at creation: the duplicates were made silently during play and reported when
the autosave reloaded. The repair's merge fails on them and its delete path is
what trips `ctx.size() >= 2`. An existing 320-tile save is therefore not
repairable — regenerate.

**The fix (`octree=1`)**: a 13-byte in-place rewrite at `0x1402304f8` that puts
`65536.0f` inline and asks for depth 11:

```
b8 00 00 80 47     mov  eax, 0x47800000    ; 65536.0f
66 0f 6e d0        movd xmm2, eax
31 d2              xor  edx, edx
b2 0b              mov  dl, 0xb            ; depth 11
```

`eax` is dead there and the length is identical, so nothing shifts. The
`32768.0f` in `.rdata` sits in a `{64, 16384, 32768, FLT_MAX, −90}` run with
~100 readers and is deliberately **not** touched. Depth 11 is a hard cap — node
indices are `int32` linear (`child = 8·parent + 1 + octant`) and the renderer's
skip-manager decoder overflows at level 11+ — so **the patched ceiling is 512
tiles (131 km)**. The leaf stays 128 m, so query behaviour is unchanged; the cost
is one extra tree level. The patch is applied only when the config asks for a
size over 256 tiles.

Byte-verified before writing, and the plugin logs exactly what it did. What is
*not* yet verified at depth 11 is the renderer's per-level skip vector sizing:
the first 320-tile map generated with this on is the test.

Terrain LOD at the edge was **not** traced to the same limit. The only
terrain-side 32,768 is an asymmetric legacy vertex packer (tiles −128..895),
which can't produce a four-edge effect. If the edge renders right with `octree=1`
and wrong without, the LOD symptom was octree-driven render culling.

### Industry founding after start

A 320-tile map placed **387 industries — exactly** `round(6711 km² × 0.0576)` —
and then kept founding more. The spawner was reversed to find out why:

- **What it counts (N):** every `Construction` entity with a non-empty
  `simBuildings` list. No filter on road connectivity, level or town distance —
  the 239 unconnected industries count fully.
- **What it targets (T):** `round(map_area_km² × targetMaxNumberPerArea)`. Whole
  map, no town term.
- **Two timers**, both registered only if `spawnIndustries` is true *and* both
  densities are positive: a fixed-cadence one (`spawnTargetTimeSpan / n`, one
  attempt per fire, logs `Connect industry`) and a daily probabilistic one
  (`p = ((T−N)/T)^spawnProbabilityExponent` per newly-supplied building, silent).
- **Both are gated strictly `N < T`.** The static code cannot found at `N ≥ T`.

So the "counts only connected" and "per-town" theories are both refuted, and the
foundings past 387 mean a runtime premise was off — most likely
`targetMaxNumberPerArea` not carrying the mod's scale. The mod now prints target
and `spawnIndustries` at startup so that is measurable rather than argued.

**The switch that works regardless:** `Industry density target: Disabled` in the
New Game menu, which sets `spawnIndustries = false` and registers neither timer.
Closures still happen (that system never reads the flag), so closed industries
are not replaced; add `closureProbability = 0` to freeze those too. Raising
road-connect success does **not** touch the founding rate — connectivity is in
neither N nor T.

### Town and industry levels: `mod/bigmap_density_1`

Counts are a **fixed density per km²**, so they scale with area. A 57 × 57 km
map is 3,288 km² — 5.4× the largest map the game ships — and generates ~1,600
industries and ~200 towns, which is neither fun nor quick to generate.

**Two multipliers are applied before ours, and neither defaults to 1.0.** This is
the easy thing to get wrong, and getting it wrong overstates every count by
1.7–3.3×:

| | base density | stock dropdown default | effective |
| --- | --- | --- | --- |
| towns | 0.2 /km² | Medium ×0.3 | 0.06 /km² |
| industries | 0.8 /km² | Medium ×0.6 | 0.48 /km² |

Town multipliers are `{ Low 0.2, Medium 0.3, High 0.4, Very high 0.5 }`, applied
**engine-side** — they are not in the shipped Lua. Read from the dispatch sites
(`0x142f304c8`=0.2, `0x142f28810`=0.3, `0x142f65170`=0.4, inline `0x3f000000`=0.5),
and independently confirmed by a real map: a 57 km map at the old hand-patched
0.0367 /km² generated **36 towns**, and `0.0367 × 0.3 × 3288 = 36.2`. That also
settles a discrepancy this README used to record as unexplained — the "36 towns
where the formula predicts 115" was the ×0.3, nothing to do with
`allowInRoughTerrain`. Industry multipliers are `{ .4, .6, .8, 1.0 }` from
`base_mod.lua:280`.

This repo ships a **Lua mod** that makes the rest a choice in the New Game menu.
Counts below are for a 57 km map with the stock dropdowns left alone:

| level | scale | towns | industries |
| --- | --- | --- | --- |
| Vanilla | ×1.00 | ~197 | ~1578 |
| Reduced | ×0.50 | ~99 | ~789 |
| Sparse | ×0.30 | ~59 | ~474 |
| **Megalomaniac count at 57 km** (default) | ×0.18 | **~36** | **~284** |
| Minimal | ×0.10 | ~20 | ~158 |
| Megalomaniac count at 115 km | ×0.046 | ~9 | ~73 |
| Megalomaniac count at 164 km | ×0.022 | ~4 | ~35 |

**A fixed multiplier does not hold a count as the map grows.** The scale needed to
keep Megalomaniac's 36 towns / 290 industries is just `604 / area`, so it falls
off ~4× every time the map's edge doubles: ×0.18 at 57 km, ×0.089 at 82 km,
×0.046 at 115 km, ×0.022 at 164 km. That is why the bottom two rungs exist and
why they are named for a size rather than a number — without them the lowest
setting still produced 631 industries at 115 km and 1,288 at 164 km. Pick the
rung that names the size you are generating:

| rung | at 320² (82 km) | at 448² (115 km) | at 640² (164 km) |
| --- | --- | --- | --- |
| ×0.18 | 72 / 580 | 142 / 1136 | 290 / 2319 |
| ×0.10 | 40 / 322 | 79 / 631 | 161 / 1288 |
| ×0.046 | 19 / 148 | **36 / 290** | 74 / 593 |
| ×0.022 | 9 / 71 | 17 / 139 | **35 / 283** |

The default is named for a measured target, not a guess: Megalomaniac 1:1 is
604 km², which gives **36 towns and 290 industries** at the same default
dropdowns. ×0.18 on a map 5.4× the size reproduces that to within 2%.

Two design points worth stating, because the obvious alternatives are worse:

**It is a mod, not an edit to `res/config/base_config.lua`.** That file is a game
file: Steam's *verify integrity of game files* reverts it and an update
overwrites it, both silently. It is also global, so values tuned for a 57 km map
make a stock-size map sparse.

**It multiplies `game.config`, it does not assign to it.** `base_mod.lua:280`
already multiplies industry density by `{.4,.6,.8,1.0}` from the stock "Number of
industries" dropdown. Multiplication commutes, so our scale and the stock
dropdown stack instead of fighting — and mod load order, which we do not control,
stops mattering. Assignment would have made it a race.

The default is ×0.18 rather than vanilla because a mod you had to tick a box named
"Big Map Density" to enable should not quietly do nothing; ×0.18 reproduces
Megalomaniac's own counts at any size. Enabling the mod *is* the opt-in — leave it
off and no map changes.

Install it like any mod: copy `mod/bigmap_density_1` into
`<game>\mods\`. Enable it when you **create** the map — density is a worldgen
setting.

### What we could not do: relabel the size dropdown

The ladder above reuses the *ratio* dropdown, so it still reads "1:1 … 1:5" while
selecting a size. That is not fixable from a mod, and the reason is worth
recording so nobody retries it:

```
res/scripts/mod.lua:165
    local txt = translateModStr(_currentModIdTr, _locale, concatId)
```

`pGetText` resolves a string against the **currently executing mod's** own table
only. A mod's `strings.lua` can retranslate strings its own Lua asks for; it can
never reach a string the base game resolves. `"1:1"` is looked up by the C++ New
Game menu under the base catalog, with no mod in scope.

The msgids are real and confirmed — `'1:1'`, `'1:2'`, `'1:3'` and
`'map-sizeMegalomaniac'` all live in `res/strings/*/LC_MESSAGES/base.mo` —
so the only ways to change them are editing `base.mo` (a game file, reverted by
Steam) or a DLL hook on the text lookup. Our *own* mod params are unaffected:
they resolve while our mod is current, so their labels are exactly what
`mod.lua` says.

The DLL route is viable if it ever becomes worth it. `0x14221d1b0` is
`pgettext(std::string* out, const char* ctx, const char* msgid)` — 172 xrefs, all
UI text, with 11 clean relocatable prologue bytes. Two traps: the `mov r11,rsp`
must be **copied into the trampoline**, not skipped, because `r11` is used later
as a frame base; and the hook must gate on the **context**, not the msgid, since
`"Tiny"`/`"Small"`/`"Large"`/`"Huge"` are the same `char*` literals the town-size
dropdown uses. Call the original first — `*out` is uninitialised on entry — then
append rather than replace, so all 13 languages keep working.

A better idea than relabelling, if the guard below gets written: claim one row
**per size** (4, 5 and 6) instead of spending the ratio dropdown. Then every
label stays honest — "Very Large < Huge < Megalomaniac" is still ordinally true
after a remap, and "1:3" still means 1:3 — with no text hook at all. Nothing on
the New Game page displays a derived tile count to contradict it:
`CreatePageNewGame` (`0x14066c2b0`) never calls `GetNumTilesNew`.

**That remap is gated on a guard this plugin does not yet have.** `Detour()`
receives the raw combo index, and at `0x140674b2f` the engine keys the preset
table on `sizeIndex` when `experimentalMapSizes` (`GlobalSettings+0x2fc`) is set
but on `sizeIndex+1` when it is clear. With the flag clear a `size4`/`size5`
claim would land on a different, stock preset — silently redefining a normal map.
The shipped ladder is safe from this only because it claims size 6, which is
unreachable unless the flag is on.

## Install

**Download `TpF2BigMaps-<version>.msi` from the
[latest release](https://github.com/silver2127/tpf2-bigmap/releases) and run it.**
It finds the Transport Fever 2 folder Steam registered, asks you to confirm it,
and puts these in place:

| file | what |
| --- | --- |
| `alut.dll` | the proxy the game loads in place of its own (the original is kept as `alut_real.dll`) |
| `tpf2_pluginhost.dll` | the plugin host the proxy loads |
| `plugins	pf2_bigmap.dll` | this plugin |
| `plugins	pf2_bigmap.cfg` | its settings — the size ladder, `octree`, `street_raster`. Never overwritten once present, so edits survive upgrades |
| `modsigmap_density_1\mod.lua` | the town/industry density mod — enable it when you **create** a map |

It also sets the Segment Heap switch for `TransportFever2.exe` (a registry
value, removed on uninstall) — that is what makes a big map load in about a
minute instead of a quarter of an hour; the measurements are in the
[multiplayer installer README](https://github.com/silver2127/tpf2-multiplayer/blob/main/installer/README.md#segment-heap).

Then: New Game → **Megalomaniac**, and the *ratio* dropdown picks the size
(see the ladder above). Tick **Big Map Density** in the mod list and pick the
rung that names the size you chose.

### Installing alongside TpF2 Multiplayer

Both packages work in either order and can be removed in either order. They
share the proxy and the plugin host, and both installers declare those under
the **same component GUIDs** (`installer/PluginHost.wxs`, byte-identical in both
repositories), so Windows Installer reference-counts them: the second install
finds them present, the first uninstall leaves them for the other, and only the
last one out puts the game's own `alut.dll` back. The custom actions that park
and restore `alut.dll` check that count too, so uninstalling one product never
restores the stock library out from under the other.

Each product keeps its own config — this one in `plugins	pf2_bigmap.cfg`, which
the host merges over `tpf2mp.cfg` — so neither installer touches a file the
other owns.

`installer	est_coexist.ps1` proves all of it against a throwaway folder with
the real `msiexec` transactions (both orders, both directions, the shared
registry value tracked and restored). It needs an elevated PowerShell because
the packages are per-machine.

### Uninstall

Add/Remove Programs → **TpF2 Big Maps**. Removes the plugin, its config, the
density mod, and — if TpF2 Multiplayer is not installed — the proxy, the
plugin host, the Segment Heap value, and restores the stock `alut.dll`. Steam's
*Verify integrity of game files* also puts the stock `alut.dll` back without
uninstalling anything; **Repair** from Add/Remove Programs reinstalls the proxy.

## Build

Needs VS 2022 Build Tools.

```
build.bat            -> out	pf2_bigmap.dll
build.bat -deploy    -> also copies into %LOCALAPPDATA%	pf2mp\data\plugins```

For a dev setup without the MSI: install the tpf2mp plugin host from the
multiplayer repository (`tpf2_pluginhost.dll` + the `alut.dll` proxy), drop
`out	pf2_bigmap.dll` and `cfg	pf2_bigmap.cfg` into `<game>\plugins\`, and
copy `modigmap_density_1` into `<game>\mods\`.

### Building the MSI

```
toolsendor_host.ps1 -Build          # copies alut.dll, tpf2_pluginhost.dll, tpf2ca.dll
                                      # from a tpf2-multiplayer checkout beside this repo,
                                      # and records the source commit in installerendor\VENDORED.md
installeruild_msi.ps1 -Validate -AcceptWixEula
```

The three shared binaries are built in the multiplayer repository and vendored
here unchanged: both packages must ship the same bytes under the same GUIDs.
`build_msi.ps1` refuses to build if `PluginHost.wxs` has drifted from the
multiplayer copy. WiX v7 asks you to accept its
[OSMF EULA](https://wixtoolset.org/osmf/); `-AcceptWixEula` passes it
per-invocation and nothing accepts it for you.

## Verifying it worked

`%LOCALAPPDATA%	pf2mp\data	pf2mp_host.log` shows the hook lines, the
`octree:` line, and `merged ...\plugins	pf2_bigmap.cfg`. Then generate, save,
and read `numTilesX`/`numTilesY` back out of the `.sav` header with the snippet
near the top of this file — that proves the value survived generation *and*
serialization, which is stronger than trusting the log.

## Licence

MIT. See `LICENSE`.
