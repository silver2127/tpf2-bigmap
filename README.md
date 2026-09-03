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

**One tile is 250 m** (64 px at 3.90625 m/px). That is not inferred — the `.sav`
header carries `numTilesX`/`numTilesY` at +0x10/+0x14 after zstd decompression,
and five different real maps all land exactly on 250 m/tile:

| tiles | km | preset |
| --- | --- | --- |
| 18 × 54 | 4.5 × 13.5 | Small 1:3 |
| 22 × 88 | 5.5 × 22 | Medium 1:4 |
| 48 × 192 | 12 × 48 | Megalomaniac 1:4 |
| 54 × 162 | 13.5 × 40.5 | Megalomaniac 1:3 |
| 66 × 132 | 16.5 × 33 | Megalomaniac 1:2 |

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

## What is actually known

| tiles | km | heightmap px | status |
| --- | --- | --- | --- |
| 96 | 24 | 6,145 | stock Megalomaniac 1:1 |
| 224 | 56 | 14,337 | the clamp — reachable with no plugin |
| 256 | 64 | **16,385** | = 2¹⁴+1, the largest dimension the heightmap-import docs quote. The most likely real wall, and the interesting data point |
| 448 | 112 | 28,673 | int32 pixel arithmetic still holds (28673² = 822M ≪ 2³¹) |

**Nothing above 224 has been shown to work.** This is the instrument for finding
out, not a claim that it does. Ladder it: 224 → 256 → 320 → 448, and note where
it stops.

Expected failure modes, in order: something assuming ≤16,385 px or ≤256 tiles
(the `terrainEcs.baseLevels < terrainEcs.highLevels` assert hints at a quadtree
depth); then terrain-tile ECS pressure (448² = 200,704 tile entities); then town
and industry placement, which scales with area.

Float precision is *not* a risk — at 112 km the world spans ±56 km, where the
float32 ULP is ~4 mm against 3.9 m terrain resolution.

## Build

Needs VS 2022 Build Tools.

```
build.bat            -> out\tpf2_bigmap.dll
build.bat -deploy    -> also copies into %LOCALAPPDATA%\tpf2mp\plugins\
```

## Install

1. Install the tpf2mp plugin host (`tpf2_pluginhost.dll` + the `alut.dll` proxy).
2. Drop `tpf2_bigmap.dll` into `%LOCALAPPDATA%\tpf2mp\plugins\`.
3. Add a `[tpf2_bigmap]` section to `tpf2mp.cfg`:

```ini
[tpf2_bigmap]
enabled=1
tiles_x=224
tiles_y=224
size_index=6      ; 0..6 = Tiny..Megalomaniac
format_index=0    ; 0..4 = 1:1..1:5
max_tiles=224     ; raise deliberately to test past the stock clamp
log=1
```

With that, picking **Megalomaniac + 1:1** in the New Game menu builds 56 × 56 km.
Every other combination keeps its stock size — the detour calls the game's own
function for anything it does not claim.

`tiles_x`/`tiles_y` default to 0, so the plugin is inert until you ask for a
size. It patches game code; it should not surprise anyone who merely installed it.

## Verifying it worked

Check `%LOCALAPPDATA%\tpf2mp\data\tpf2mp_host.log` for the hook line, then
generate, save, and read `numTilesX`/`numTilesY` back out of the `.sav` header
with the snippet above. That is a stronger check than trusting the log: it
proves the value survived generation *and* serialization.

## Licence

MIT. See `LICENSE`.
