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

## The real ceiling: 184 tiles (46 × 46 km) — MEASURED

Not the game's 224 clamp. **224 crashes**, and here is exactly why.

The "Creating streets" pass builds a **1-metre occupancy raster over the whole
map bounding box** and sizes it with a 32-bit signed multiply (RVA `0x90d410`):

```
mov    eax, [rbx+0x44]          ; ny
imul   eax, dword [rbx+0x40]    ; nx * ny   <-- 32-bit signed
movsxd rdx, eax                 ; sign-extend into size_t
call   vector<bool>::resize
```

At 224 tiles the map is 56,000 m per side:

```
56,001 x 56,001 = 3,136,112,001  >  INT_MAX (2,147,483,647)
                -> -1,158,855,295 as int32
                -> sign-extended to ~1.8e19
                -> std::length_error("vector<bool> too long")
                -> uncaught -> std::terminate -> abort -> SIGABRT
```

No message is printed because it is an **uncaught C++ exception, not an
assert** — the game's assert handler never runs. Confirmed by resolving the
thrown object's RTTI in the minidump: `.?AVlength_error@std@@`.

**The rule:** `(width_m + 1) * (height_m + 1) <= 2147483647`, i.e. ≤ 46,339 m on
a square map.

| tiles | km | (m+1)² | status |
| --- | --- | --- | --- |
| 96 | 24 | 0.037e9 | stock Megalomaniac 1:1 |
| **184** | **46** | **2.116e9** | **largest even square that fits** |
| 185 | 46.25 | 2.139e9 | fits, but odd (the engine requires even) |
| 186 | 46.5 | 2.162e9 | **overflows** |
| 224 | 56 | 3.136e9 | the game's own clamp — **crashes** |

184 × 184 km is still **3.7× the largest stock map by area** (2,116 km² vs 576).

Non-square gets more in one axis under the same product rule — `300 × 114` tiles
(75 × 28.5 km) is legal, `186 × 186` is not.

Float precision is *not* a risk at any of these sizes — at 46 km the world spans
±23 km, where the float32 ULP is ~2 mm against 3.9 m terrain resolution.

### Generation cost

Towns and industries are placed at a fixed density per km²
(`res/config/base_config.lua`: `town.maxNumberPerArea = 0.2`,
`industry.maxNumberPerArea = 0.8`), so both counts are **strictly linear in
area** — about 423 towns and 1,693 industries at 46 × 46 km. Industry placement
is single-threaded and roughly O(N²); the parallel CPU burn is the towns and
streets passes. If generation is too slow, lowering those two density values is
the highest-value knob, and it is plain Lua config rather than a patch.

## Build

Needs VS 2022 Build Tools.

```
build.bat            -> out\tpf2_bigmap.dll
build.bat -deploy    -> also copies into %LOCALAPPDATA%\tpf2mp\data\plugins\
```

## Install

1. Install the tpf2mp plugin host (`tpf2_pluginhost.dll` + the `alut.dll` proxy).
2. Drop `tpf2_bigmap.dll` into `%LOCALAPPDATA%\tpf2mp\data\plugins\`
   (or `<game>\plugins\` for a shipped install — the host scans both).
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
