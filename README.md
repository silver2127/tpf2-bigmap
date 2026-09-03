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

## The 184-tile wall, and how the plugin gets past it

The game's own 224-tile clamp is not reachable with stock code. "Creating
streets" allocates a `std::vector<bool>` with **one bit per square metre** over
the whole map bounding box, and sizes it with a 32-bit multiply (RVA `0x90d410`):

```
mov    eax, [rbx+0x44]          ; ny
imul   eax, dword [rbx+0x40]    ; nx * ny   <-- 32-bit signed
movsxd rdx, eax                 ; sign-extend into size_t
call   vector<bool>::resize
```

At 224 tiles the map is 56,000 m per side, so `56,001² = 3,136,112,001 > INT_MAX`.
It wraps negative, sign-extends to ~1.8e19, and `resize` throws
`std::length_error`. Nothing catches it: `terminate` → `abort` → SIGABRT with no
message, because it is an **uncaught C++ exception, not an assert**. Confirmed by
resolving the thrown object's RTTI in the minidump (`.?AVlength_error@std@@`).

Stock ceiling: `(width_m + 1) × (height_m + 1) ≤ 2,147,483,647`, i.e. **184 tiles
(46 km)** on a square map. 186 overflows.

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
| 24 km (stock) | 1 m | 0.58e9 | 27% | 72 MB — **untouched** |
| 56 km | 2 m | 0.78e9 | 37% | 98 MB |
| 112 km | 3 m | 1.39e9 | 65% | 174 MB |

Below the budget it is a no-op, so normal maps keep their 1 m grid and behave
exactly as before. The cost above it is road-placement granularity — 2 m instead
of 1 m, against roads 10–20 m wide.

### Generation cost

Towns and industries are placed at a **fixed density per km²**, so both counts
are strictly linear in area (`res/config/base_config.lua`, consumer decompiled at
RVA `0x35f480`):

```lua
town.maxNumberPerArea     = 0.2   -- km^-2
industry.maxNumberPerArea = 0.8   -- km^-2
```

At stock density a 56 × 56 km map generates **627 towns and 2,509 industries** —
5.4× the largest map anyone has ever played. That is not just slow, it is
probably not the map you want: the point of a big map is more room per industry,
not more industries.

To get the largest stock map's *counts* spread over 56 × 56 km instead:

```lua
town.maxNumberPerArea     = 0.0367   -- 627  -> ~115 towns
industry.maxNumberPerArea = 0.147    -- 2509 -> ~461 industries
targetMaxNumberPerArea    = 0.147
```

Industry placement is single-threaded and roughly O(N²)
(`0.1·N² · tags · placed`), so a 5.4× count reduction is a ~30× cut in that
pass. The *parallel* burn is the towns and streets passes (27 and 13
thread-pool functions reachable; industries reaches none).

These are plain Lua config, not patches — but note they are **game files**, so
Steam's "verify integrity" will revert them.

Stock counts for reference, 1:1 (computed from the formula, not observed):

| size | km | towns | industries |
| --- | --- | --- | --- |
| Small | 8 | 13 | 51 |
| Large | 14 | 39 | 157 |
| Megalomaniac | 24 | 115 | 461 |

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
