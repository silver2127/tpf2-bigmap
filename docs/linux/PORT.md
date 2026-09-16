# Linux native port, build 35924

Baseline Windows source: bigmap commit `4f0de6f` (0.4.0).
Linux ELF GNU build-id: `3a0e156390b0e6f1e372051c24802c8493ae454a`.
The host checks this identity; the plugin checks every patch before publishing
any change. `tools/linux/verify_game.py GAME_ELF` independently checks all sites.

## Reverse-engineering evidence

The local Linux function/signature/xref exports were cross-checked against
actual x86-64 disassembly of the Steam ELF. Addresses below are Linux RVAs.

| Site | Evidence and ABI |
| --- | --- |
| `0x11232c0` GetNumTilesNew | Signature in MenuUI.cpp; `edi=size`, `esi=format`, `rdx=AppConfig`. Returns x/y packed in rax. Override at +0x28/+0x2c and 224 clamps at 0x112330b/0x1123323. First 18 bytes have no relative operands. |
| `0x14e05a0` occupancy raster ctor | `rdi=this`, `rsi=Box2`, `xmm0=cellSize`; stores box at +8, cell at +0x18, vector<bool> at +0x20, nx/ny at +0x48/+0x4c. `imul ecx,eax` at 0x14e0644, then sign-extension before allocation. Four direct callers are redirected; no Windows layout assumptions or relocation of its RIP-relative prologue. |
| `0xa84234` octree upper tier | Caller at 0xa84040 checks either tile axis >128; loads 32768 into xmm0 and depth 10 into esi, then calls 0x16a6580. Resize stores symmetric bounds at +8..+0x1c and depth at +0x20. Replace the RIP-relative constant with a private nearby 65536 and depth 11; no shared constant is edited. |
| `0x1149945` size combo call | New Game builds four configured labels or seven experimental labels, then passes their vector as rdi to 0x31443a0. The factory reads/copies strings into its widget; the wrapper passes a borrowed libstdc++ vector view and never changes allocator ownership. |
| `0x1149ae7` ratio formatter call | Hidden result string in rdi, ratio index in esi. Return a constructed 32-byte libstdc++ SSO string. Extend loop bound at 0x1149bf4 and enable gate at 0x1149c01. Other formatter callers are untouched. |

The raster call sites are 0x1066d80, 0x150af14, 0x151e175 and 0x15269cf.
Each retains the original SysV calling convention through a nearby absolute
jump stub. Stub pages are written RW then made RX. mmap uses
MAP_FIXED_NOREPLACE; existing mappings are never displaced.

All patches are preflighted. The size trampoline is installed before any UI
extension; failed patch application attempts rollback. Code pages remain
mapped, so no published branch targets freed memory. With raster disabled,
menu additions cap at 180 tiles; without octree expansion they cap at 256.
The heightmap-coordinate diagonal squared is also kept below INT_MAX: 512x512
would cross that boundary in the stock placement-distance calculation, so a
derived square becomes 510x510. A 512-tile edge remains available on rectangles.

## Build and package

`tools/linux/build.sh` uses Valve's pinned soldier SDK
2.0.20260805.254767 (GCC 8.3, glibc 2.31 baseline). Set TPF2MP_SDK_ROOT if
installed elsewhere. Native exports are limited to Tpf2mpPluginInit.

`tools/linux/package.sh /path/to/tpf2_pluginhost.so` packages the plugin and
shared host. The initial host comes from tpf2-multiplayer Linux commit
`070f96a` / v0.5.6-linux-dev.1. Its ABI header is already vendored in src/;
there is no build-time coupling. BUILDINFO records the host SHA-256. The
installer preserves an existing host and multiplayer launcher.

## Tests and limits

Unit checks cover stock/additional rows, every extended ratio, bounds, malformed
config, float raster sizing, unsupported builds/depths, and patch rollback.
The read-only ELF check covers all ten patched sites. Runtime host loading and
byte verification have succeeded in the isolated native multiplayer lab.

This initial port deliberately covers large-map creation through depth 11.
It is not full Windows 0.4.0 feature parity. Windows placeholder mappings and
vectored exception handlers in the terrain/material pagers need a separate
Linux memory-management design and stress tests. The optional generation,
renderer, save-speed and depth-12/13 hooks also require independent Linux RE.
The configured maximum has not been stress-tested; this is an experimental build.

The isolated native game reached New Game with the shared host alone. The
additional size labels and extended ratio labels render correctly; selecting
32.77 x 32.77 km produced a terrain preview. A 40 x 400-tile world
(10.24 x 102.4 km, beyond the stock 256-tile octree boundary) completed
generation, entered gameplay, advanced the date, and saved successfully.
The saved world reloaded successfully and simulation continued afterward.
The lab retained its multiplayer Lua mod, while
only the native shared plugin host was preloaded; this was not a two-peer
synchronization test.
