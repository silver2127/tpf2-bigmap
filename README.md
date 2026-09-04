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

### Town and industry levels: `mod/bigmap_density_1`

Counts are a **fixed density per km²**, so they scale with area. At stock density
(0.2 towns, 0.8 industries per km²) a 57 × 57 km map generates ~660 towns and
~2,600 industries — 5.4× the largest map the game ships, which is neither fun nor
quick to generate.

This repo ships a **Lua mod** that makes that a choice in the New Game menu:

| level | scale | towns @ 57 km | industries @ 57 km |
| --- | --- | --- | --- |
| Vanilla | ×1.00 | ~660 | ~2630 |
| Reduced | ×0.50 | ~330 | ~1320 |
| Sparse | ×0.30 | ~200 | ~790 |
| **Megalomaniac count** (default) | ×0.18 | ~120 | ~470 |
| Minimal | ×0.10 | ~66 | ~260 |

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
Steam) or a DLL hook on the text lookup. Neither is worth it for a cosmetic
label. Our *own* mod params are unaffected: they resolve while our mod is
current, so their labels are exactly what `mod.lua` says.

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
