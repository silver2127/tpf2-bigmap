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
The read-only ELF check covers all twenty guarded sites. Runtime host loading and
byte verification have succeeded in the isolated native multiplayer lab.

The Linux port covers large-map creation through depth 11, sparse density presets,
lossless terrain compression, faster saves and a SIMD terrain min/max scan.
It is not full Windows 0.4.0 feature parity. Material paging, terrain copy sharing,
generation buffer reuse, placement budgets, material-index/refinement/alignment
optimizations and depth-12/13 hooks still need independent Linux work.
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


## Linux sparse density port (dev.2)

The town selection is loaded from `[rbx+0x18]->+0x460` at 0x112e2ef.
Cases 0/1/2 branch to 0.2/0.3/0.4 stores at 0x112e720, 0x112e770 and
0x112e788. The 25-byte default/case-3 tail at 0x112e313 is redirected to
a private RX stub; it stores xmm3 at `[rbp-0x1bc]` and resumes at 0x112e32c.
The stub preserves rax/rdx, keeps case 3 at 0.5, handles indices 4..9 with
0.3 times the six Windows scales, and keeps the unknown-index default at 1.0.
An assembly harness executes the actual emitted stub for every new index.

Industries use the same six scales times the stock Medium multiplier 0.6.
Anchored edits extend all three lists and `industryFreq` in base_mod.lua.
The industry start index is zero-based; the target includes Disabled first,
so runFn reads `industryFreq[start+1]` and `industryFreq[target]` respectively.
Atomic file replacement and an exact-patch comparison protect backups and
subsequent user edits. Unit tests cover repeat application, restoration,
changed anchors, and Steam replacing the game file. The restoration helper
is bundled with the installer and called before uninstalling the plugin.

Live dev.2 checks: all three menus show the six new entries. On the same Small
map and seed, Medium previewed 4 towns/32 industries; Minimal previewed
2 towns/5 industries (the generator's minimum counts apply). A 128x128-tile
Minimal map previewed 6 towns/52 industries and completed world generation.
The generation log confirmed saved industry start/target indices 7/8 and
multipliers 0.06/0.06. Saving and reloading succeeded, retaining those
multipliers and continuing simulation. Stock Medium is 0.6. This test does not measure long-term
industry spawning or multiplayer synchronization.

## Linux memory/performance port (dev.3)

### Terrain cache

The Windows `TerrainCodec` format 3 is reused unchanged. It encodes all 66,049
uint16 samples losslessly; decoding verifies the stored hash. Linux uses a
new `userfaultfd` backend rather than Windows placeholder mappings or exception
handlers. A 135,168-byte page-aligned slot holds one 132,098-byte terrain vector.
The pool reserves 1,048,576 slots virtually; unallocated slots consume no terrain
pages. Slot metadata is separate (about 80 MiB).

Allocation is intercepted only at CTerrain::AddTile's vector append call
`0xcf7696 -> 0xadb7e0` and detached-copy allocation call
`0xcf7783 -> 0x6dbce0`. The shared-vector control block's dispose function at
`0xcf7bb0` owns release (vector begins at control+16). The control block's own
allocator/destructor remains unchanged. Vectors outside the pool retain the
stock allocator/free path. Growth beyond a tile migrates to a stock vector.

A policy thread write-protects candidate tiles, compresses their stable contents,
then discards their original pages. The fault thread restores and verifies the
whole tile, removes protection and wakes blocked users at the original address.
The soft hot budget defaults to 1024 MiB, with a two-second grace period and a
bounded round-robin scan. It is not an LRU cache: ordinary reads of resident
pages do not refresh their age. Compression can cost CPU and introduce latency;
no frame-rate gain is claimed. Poorly compressible tiles stay resident.

No allocation hooks are enabled if userfaultfd setup fails. The backend uses
`UFFD_USER_MODE_ONLY`, requiring no sysctl changes on the tested system. This
mode cannot service kernel-origin faults: kernel access to a missing terrain
page can produce SIGBUS. See the [Linux userfaultfd documentation](https://docs.kernel.org/admin-guide/mm/userfaultfd.html).
The game paths tested here perform terrain reads/writes in userspace; this is
not proof for every graphics driver, mod or engine path. Compression therefore
remains opt-in. Material grids, copy-on-write sharing and multi-worker restores
are not implemented in this backend. Terrain resolution remains 1 m; the
abandoned 2 m Windows cache mode is not ported.

### Save stream and terrain scan

Linux save compression setup is `0xc7b4d0`, called by SaveGame at `0xc7f22c`.
The load of level 3 at `0xc7b524` becomes a local immediate level 1. Buffer
comparison `0xc7b6b1`, allocation `0xc7c3a0`, and size store `0xc7c3aa` change
128 bytes to 64 KiB together. No shared constant is changed and the zstd/save
format stays compatible. Larger compressed files are a possible tradeoff.

The scalar uint16 min/max loop at `0xcf5852..0xcf588c` is replaced by an SSE2
scan. At entry rbx/r14 delimit the samples; at exit eax/edx hold min/max and
rbx is the end. The bridge preserves surrounding live registers and xmm2's
scale. Stock empty-vector handling and float conversion remain intact. XORing
the sign bit permits exact unsigned ordering with SSE2 signed min/max. The
Windows block-copy optimization is not part of this change.

### Validation

- Soldier SDK build passes all four CTest suites: hook/config/assembly bridge,
  density, userfaultfd pager, and shared terrain codec.
- Pager tests verify actual page eviction with mincore, concurrent exact
  restores, writes racing eviction, release/reuse and zero initialization.
  Unsupported kernels skip this test explicitly rather than reporting a pass.
- Native ELF build-id and all twenty guarded sites match build 35924.
- Live test: load the existing 128x128-tile sparse world (6 towns, 52 industries),
  render and simulate, construct two joined tracks, save as `Linux pager rail
  test`, then reload and visually verify the track and terrain. Pager restores
  remained hash-checked throughout. A fresh process then reloaded the same save
  with all three new optimizations disabled, confirming the stock paths still
  read it and retain the constructed tracks. The save log reported 879 ms; this is not a
  before/after save-speed benchmark.
- Separate fresh processes loaded the same original save with dev.2 and the
  dev.3 features. One RSS sample per second; median of seconds 60..120 was
  7.781 GiB vs 4.980 GiB. Sampled peaks were 9.851 GiB vs 9.770 GiB. Neither run
  used swap. This single-run comparison is evidence of settled memory savings,
  not reduced loading peaks or a repeatable performance benchmark. The pager
  later held 16,384 live tiles at roughly 1,024 MiB resident plus 106 MiB packed.
- Testing used the isolated native lab with the multiplayer Lua mod retained,
  but no second peer. Multiplayer synchronization and long-session stability
  remain untested for these additions.

## Minimap integration cdfee2a (partial)

Windows source integrated: `cdfee2a12511b05a2a7f67bc67ca3a818e140feb`
(2026-09-16). The Windows implementation, Lua GUI, embedding tool and tests
merge unchanged. **The native Linux minimap is not implemented.** Linux does
not install the GUI script or intercept its image tokens. `minimap=0` remains
the default; `minimap=1` logs an explicit unsupported-feature diagnostic and
leaves the existing large-map features available. No new game patches are
installed, and the existing twenty-site verification manifest is unchanged.

### Static investigation

The Linux signature/xref exports and actual build 35924 ELF were examined with
Capstone. All following addresses are investigation evidence, not enabled hook
sites. They must not be treated as a completed port or substituted into the
Windows implementation.

| Linux RVA | Established evidence |
| --- | --- |
| `0xcf5f20` | Signature identifies `CTerrain::BaseGetVertices(CVec2i) const`. `rdi` is terrain, `rsi` packs x/y. At `0xcf5f38`, `48 8b 57 18` loads terrain+0x18. Instructions at `0xcf5f4b..0xcf5f6e` subtract header x/y at +0/+4, multiply by header width +8, load cells at +0x10, use a 40-byte cell stride and check entity -1. |
| `0x1046540` | Signature identifies `CGameUI::CreateConstructionMenu`; SysV this is rdi. Entry bytes `f3 0f 1e fa 55 48 89 e5 41 57 41 56 41 55`. At `0x10465dc`, `48 8b 87 50 04 00 00` loads UI+0x450. This alone does not establish the accessor's vtable or the returned state layout. |
| `0x30b26c0` | Signature identifies raw `ImageView::SetImage(int,int,int,const vector<unsigned char>&,bool)`. Prologue saves channels from esi, width from edx, height from ecx, vector from r8 and bool from r9d. It loads the vector's begin pointer and passes it to `0x34c65a0` at `0x30b281e`. Texture upload/copy lifetime has not been established through that callee. |
| `0x30b2ab0` | Nearby candidate examined for the image-path overload. It saves this from rdi, argument pointer from rsi and flag from edx. However, at `0x30b2b6e..0x30b2bb2` it reads string data/length at argument+0/+8 **and** +0x20/+0x28, then a field at +0x40. Thus it is not proven to accept the single-string argument used by the Windows detour. Do not hook it with a libstdc++ string-only prototype. |

The adjacent raw-image constructor at `0x30b2950` calls `0x30b26c0`.
Terrain code near `0xcf6474..0xcf6495` reads levels at +0x28 and resolution
at +0x2c/+0x30, consistent with part of the Windows sampling layout, but this
is insufficient to certify the entire terrain sampler. Searching TerrainPtr
signatures also led to the ImportHeightmap callback at `0x10a1190`; its
0x450 field is a widget double, not evidence for CGameUI's terrain accessor.

### Not ported / evidence still required

- Resolve the Lua `ImageView:setImage` binding to the actual Linux string
  overload and prove its argument ownership and delegation to the image
  resource overload. The candidate above failed the single-string check.
- Trace UI+0x450 through the Linux accessor and current game state to terrain;
  prove the virtual slot and terrain offset instead of importing Windows
  vtable slot 1/state+0x20. Check replacement across loading another world.
- Complete the terrain origin/height-scale/water-field evidence and raw texture
  upload lifetime before calling the sampler or freeing uploaded pixel data.
- Then implement Linux guarded hooks, script synchronization and native renderer
  integration, with synthetic ABI/render tests. The merged Windows renderer
  and Windows ctypes/DLL tests are not a native Linux implementation.

Validation for this partial integration: `tools/linux/build.sh` and
`python3 tools/linux/verify_game.py GAME_ELF`. The native config test checks
that requesting the unsupported minimap logs a warning and writes only the same existing patch sites and lengths as the default configuration. No live game
validation was attempted, as required by this job.

## Minimap company map integration d99c054 (shared code only)

Windows source integrated: `d99c0549793721d9dc17c67ec28a1f9161ac0398`
(2026-09-16, "minimap: company colours and names from the multiplayer mod's
map"). The commit changes only the shared Lua GUI
(`mod/minimap/bigmap_minimap.lua`) and the Windows minimap test
(`tools/test_minimap.py`). Both merge unchanged. The GUI now reads
`mp_company_map.txt` (`me=<cid>`, then `<cid>=<player>=<percent-escaped name>`)
beside `mp_company_cfg.txt` and uses the creation-order guess only when that
file is absent.

The commit has no platform-specific code: no hook, byte pattern, address or
struct offset. So there is nothing to reverse engineer, no Linux patch site is
added and the twenty-site manifest is unchanged. As recorded for `cdfee2a`
above, native Linux still does not install the minimap GUI or implement its
renderer, so this change has no effect on Linux yet. It will apply unchanged
once the minimap is ported.

Validation: `tools/linux/build.sh` (four CTest suites pass) and
`tools/linux/verify_game.py` (build-id and 20 sites pass). The merged script
parses under system Lua 5.2 (`luac -p`). A standalone run of the new
map-file parser on the test's sample file gave the expected player-to-company
mapping, local company and unescaped names. `tools/test_minimap.py` was not run
because it loads the Windows `out/tpf2_bigmap.dll` and needs `pefile`/`lupa`.

## Windows integration bd0d85f (Linux dev.4, partial)

Integrated on 2026-09-22: the oldest twenty Windows commits, `6121934` through
`bd0d85f2b01f9564f6a9b77aecef684d7e0026af`. The original baseline above remains
historical; this is an incremental merge into `linux-native`. No conflicts were
present. Windows sources and tests are preserved. The merge is staged, not
committed. The remaining 27 Windows commits are outside this integration.

| Windows commits | Native disposition |
| --- | --- |
| `6121934`, `a75f83e`, `ae3027e` | Native terrain deduplication and diagnostic hash-group census; Windows measurement documentation merged unchanged. |
| `4b39aad`, `a0805be`, `5271b75`, `48946d2`, `c4aa846`, `09fd641`, `35317e7` | Working-set/loading budgets, section retry, commit pressure and adaptive eviction policy remain unported; investigation below. |
| `101d36e` | Both travel-time controls ported to verified Linux data cells. |
| `9431584` | Lazy zero terrain allocations through userfaultfd missing-page handling. |
| `8f4e6a9`, `52ede02`, `cce148a`, `0d2c47e`, `b528366` | Block routing, block diagnostics, small pager and commit backpressure remain unported. Shared block codec builds and is tested natively. |
| `c76db05`, `30cb13a`, `bd0d85f` | Batching implementation and corrected 32-byte values retained for Windows; Linux tree/lifetime investigation below. Windows measurements are not Linux results. |

### Native pager changes

`terrain_lazy_zero=1` leaves a newly allocated slot missing, with explicit zero
metadata and no blob. Its first read or write copies one zero-filled stride using
UFFDIO_COPY, then removes write protection and wakes the faulting threads.
Untouched release does not restore pages. `terrain_lazy_zero=0` retains eager
UFFDIO_ZEROPAGE initialization. Both preserve the existing CTerrain allocation
and release ABI; no new game allocation hooks or offsets are introduced.

`terrain_dedup=1` indexes immutable packed blobs by the codec hash. Before sharing,
the policy worker decodes a candidate and compares **all 132,098 sample bytes**
against the write-protected resident tile. Hash collisions cannot authorize a
false share. A hit takes a reference and skips encoding; a miss creates a blob.
The index is non-owning, removes the entry on final release, and uses a separate
mutex with slot-before-index lock ordering. Restore always creates private
resident pages and drops its blob reference, even for a read. This differs from
Windows' retained read-only restored views, but preserves exact contents and
independent lifetimes. Packed byte statistics count shared mappings once.
Index/metadata allocation failure refuses that eviction and unprotects the tile.

`terrain_dedup_probe=1` reports hashed, skipped, distinct, duplicate, zero, pair
and largest-group counts every 120 seconds. Cold tiles use their stored hashes;
resident tiles are write-protected during hashing; lazy zeros need no restore.
This is a diagnostic census of hash groups, not an atomic world snapshot or an
exact equality proof. Unlike Windows, there is no ten-second loading cadence or
low-half diagnostic. There is no verified native loading signal. Probing can
stall writers and defaults off. Dedup and lazy zero default on **only inside the
opt-in terrain compression backend**; terrain compression itself still defaults
off. No Linux load-speed or memory-saving measurement is claimed for dev.4.

### Travel-time RE and guards

The actual ELF's `.rodata` has the same two float values at different addresses:

| Linux RVA | Stock bytes | Setting / consumers |
| --- | --- | --- |
| `0x43029a8` | `00 00 96 44` (1200.f) | `travel_time_limit_s`: PathFactory, destination scoring, reachable-station BFS |
| `0x43029a4` | `00 80 bb 45` (6000.f) | `cargo_path_time_s`: PathFactory and StockListSystem |

A complete `objdump -d -M intel` scan found four RIP-relative readers of 1200:
`0x14e1779`, `0x14fe79e`, `0x1500013`, `0x1500233`; and two of 6000:
`0x14e178d`, `0x172cc06`. Their instruction bytes respectively are
`f3 0f 10 15 27 12 e2 02`, `f3 0f 10 2d 02 42 e0 02`,
`f3 0f 10 05 8d 29 e0 02`, `f3 0f 10 05 6d 27 e0 02`,
`f3 0f 10 25 0f 12 e2 02`, `f3 0f 10 05 96 5d bd 02`.
The reader instructions are evidence, not patched sites.

PathFactory.cpp assert/signature references anchor `0x14e13c0`: it loads 1200
into xmm2, stores the stack limit at rbp-0xbb8, conditionally replaces it with
6000 from xmm4, then passes that limit in xmm0 at `0x14e1825`.
The destination_util.cpp function `0x14fe200` compares accumulated time against
1200 at `0x14fe7a6`. The two destination tasks at `0x14fff50`/`0x1500170` pass
1200 in xmm0 to `0x1558870` at `0x1500022`/`0x1500242`. That callee's source and
signature references at `0x155953c`/`0x1559548` identify
`simulation_util::path_finder::GetReachableStationsBFS(int,float,...)`.
StockListSystem.cpp signatures identify `0x172c970` as `Produce`; its load at
`0x172cc06` supplies 6000 in xmm0. These independently establish the shared
constants' roles without importing Windows addresses or struct layouts.

Only the two four-byte data cells are changed. SysV floating arguments continue
through the stock instructions; no trampoline, object layout or ownership
contract changes. Values <=0 leave stock; positive settings clamp to 60..86400.
Both sites participate in the existing verify-all-before-write plan and rollback.
A mismatch refuses initialization before publishing patches. The ELF manifest
now has 22 sites. Live gameplay effects remain untested (launch failure below).

### Not ported: static attempts and missing live proof

**Alignment batching.** Signature exports identify `0x173cbe0` as the thread-pool
loop for `TerrainAlignmentSystem::UpdateSubterrains(const std::map<CVec2i,
std::vector<Box2>>&)`. Actual disassembly traces its caller to `0x173dae0`:
`rdi=self` is saved in r14 at `0x173daec`, `rsi=map` in r12 at `0x173daf3`.
It reads the leftmost node at map+0x18 (`0x173db87`), uses map+8 as sentinel
(`0x173db8c`), and calls libstdc++ `_Rb_tree_increment` at `0x173e018`.
Its direct caller at `0x173e443` has bytes `e8 98 f6 ff ff`. The loop call is
`0x173e0a6 -> 0x173cbe0`; publication calls `0xcf56d0` at `0x173e16f`,
followed by delete calls including `0x173e180` and `0x173e1a9`.
The caller clears the tree rooted at self+0xa0 (header self+0xa8, count +0xc8).
This is not the MSVC head/isnil layout. No fake native tree is published.
Still required: live map nodes and full 32-byte values, vector ownership across
thread-pool completion, and proof that publishing one batch cannot affect the
computation of later batches. The lab launch failed before these probes could
attach, so `alignment_batch_tiles` remains disabled with a request diagnostic.

**Work/result blocks and small pager.** Source/signature anchors and disassembly
locate Linux `terrain_util::GetBlock` at `0xdb55c0`. It squares the dimension at
`0xdb587b`, doubles the sample count at `0xdb5881`, and calls operator new
(`0x6dbce0`) with bytes in rdi at `0xdb589e` (`e8 3d 64 92 ff`). It stores the
three pointers at rbp-0xa0/-0x98/-0x90 and zeros the uint16 samples in a loop.
A delete call occurs at `0xdb5bfe -> 0x6dbcd0`. Windows' vector-constructor
return-address filter and CRT free-IAT hook therefore cannot simply be moved.
The native publication helper `0x173ed50` was also disassembled; it deletes at
`0x173ee94`. Allocation escape/exception paths, all result resizes and final
owners are not proven. Without a live load to observe them, intercepting global
delete could hand arena pointers to an unguarded native free. `terrain_blocks`
remains disabled with a diagnostic; its counters, small-span pager, fixed budget
and pressure throttle are absent. The portable `BlockCodec` is retained and
validated independently; that does not claim that the small pager is ported.

**Budgets, rate control and backpressure.** The Windows implementation relies on
GlobalMemoryStatusEx free commit, page-file-backed section creation, world-entry
state, bulk allocation timestamps and the menu DLL's `Tpf2mpLastGameUiTick`.
The Linux source and installed menu `.so` exports were examined: no corresponding
export exists (game UI/frame state is local). Linux uses anonymous
MAP_NORESERVE memory and UFFDIO_COPY, not section creation. This host reports
`vm.overcommit_memory=0`, `overcommit_ratio=50`; CommitLimit minus Committed_AS
is not the Windows section-allocation contract. Importing the 10/12 GiB thresholds
without validating pressure and fault progress would misrepresent protection
against OOM. No native load/pressure/frame measurements were possible after the
lab failed. The Linux pager retains its fixed hot budget, two-second age and
bounded scan; there is no loading allowance to ramp down, measured-cost/frame
rate cap, pressure-driven cap override, restore retry or backpressure. Explicit
Windows warm/rate settings log a diagnostic. Material paging was already absent;
its matching policy changes remain absent too. Needed next: a native gameplay
stamp/loading signal, validated Linux memory-pressure inputs and partial-copy
failure/retry tests under a constrained lab process before enabling the policy.

### Build, tests and live attempt

`tools/linux/build.sh` passes five suites under the soldier SDK: bigmap, density,
pager, terrain codec and the new shared block codec suite. The userfaultfd test
actually ran, rather than skipping. New checks cover untouched page residency,
first-read/write zeros, untouched release, eager opt-out, cold and resident
censuses, sixteen shared pairs, private writes to twins, concurrent restores,
write/eviction races and final blob cleanup. Travel tests cover clamping, disabled
values, byte mismatch before writes and rollback after a failed data write.
The block codec checks zero/ramp/random data, boundary sizes, truncation, hash
corruption and capacity rejection. ELF build-id and all 22 patch sites pass.

The lab's share/tpf2mp and mp_lockstep_1 directories were backed up with `cp -a`
to `.before-port`. The built plugin and test config were installed only in the
native actor. The official launcher at
`/home/topsnek/tpf2-multiplayer/tools/sandbox/tpf2mp-lab` was used because this
bigmap clone has no sandbox launcher. It failed immediately with
`bwrap: setting up uid map: Permission denied`. Two attempts using the same lab
mount/launch specification with system bwrap (including PRESSURE_VESSEL_BWRAP)
reached pressure-vessel but failed to create its nested namespace. A final
attempt with the final build through the original launcher failed identically.
No kernel/security policy was changed. No game process, title menu, renderer,
Vulkan device, save load or gdb session was reached; no live success is claimed.
All attempted runs exited immediately, below the three-minute limit. No Steam
process was restarted, no save was modified, and no input events were sent.

The actor share tree was restored from its backup; the unchanged mod tree was
compared to its backup. Recursive comparisons are empty. No launched process
remains. Raw disassembly, launch errors, build/verification logs, actor logs/data,
test configuration and restore comparisons are retained in this job's
`meta/live/`. Existing actor log contents predate these failed launches and
must not be mistaken for new gameplay observations.

## Windows integration 26bced4 (partial)

Integrated 2026-09-22: twenty Windows commits `dc7264d` through
`26bced4b98893809bf9bd7960a0acec7b9ab244a`, based on the preceding `bd0d85f`
integration. No merge conflicts. Seven pending Windows commits remain outside
this batch. Windows implementations, release assets and MSI CI are retained;
the merge is staged and uncommitted.

| Commits | Native disposition |
| --- | --- |
| `dc7264d`, `915aec1` | Alignment handle, pass timing and test merge; native batching remains unported. |
| `e62e43b`, `0511bb5`, `96340f9`, `059f2f1`, `da9d033`, `ff2fd60` | Shared sidecar file API now compiles and is tested on Linux; engine grid adapter and save/load integration are not enabled. |
| `1c3c713`, `4d696f3` | Windows RE/measurements merge as documentation, not native evidence. |
| `5bf8aaf`, `8e42635` | AddTile serving, publication suppression and pass-end release remain unported. |
| `f7c6395`, `585921b`, `a448c32` | Windows releases retained. Native minimap remains off/unavailable; pager policy portion remains unported. |
| `2d03251`, `7407127`, `ce3feb7`, `24a020a` | Adaptive caps, simulated memory, throttle hysteresis and working-set floor remain unported. |
| `26bced4` | Windows MSI/draft-release workflow retained unchanged; no publishing performed. |

### Portable sidecar validation

`src/terrain_sidecar.h` uses scoped `OpenFile`/`CompareExtension` helpers:
MSVC keeps `fopen_s`/`_stricmp`; Linux uses `fopen`/`strcasecmp`. Test exports
retain `dllexport` on Windows. The file format, fingerprint, streaming index,
lazy per-tile decode and fallback contract remain unchanged. No Windows game
address is used by the native plugin. This header's raw `GridOf`/`VectorOf`
still describe the **Windows** layout; it is compiled only by an offline native
test, not included by the native plugin.

Both incoming synthetic tests allocated only 0x20 bytes for a control block
with a 24-byte vector at +0x10: writing `end` overran it by eight bytes. They
now allocate 0x28. The sidecar test is a sixth CTest suite, with assertions
explicitly enabled in Release. It restores 824 tiles exactly, checks holes,
wrong-sized caches, mismatched fingerprints/dimensions, truncation, corrupted
blobs, per-tile fallback, windowed indices, and changed-save rejection. The
same test also passes host ASan/UBSan. This validates the shared file API with
synthetic Windows-layout objects, not Linux game ownership.

### Static RE: Linux differs at the shared pointer

Examined the actual build 35924 ELF (build-id remains
`3a0e156390b0e6f1e372051c24802c8493ae454a`) with objdump and the signature
exports. These are investigation sites, **not new patches**:

| Site | Evidence |
| --- | --- |
| `0x173db00` | `48 8b 7f 08`, loads system+8 into rdi, followed at `0x173db13` by call to signature-identified `CTerrain::GetTileCache` (`0xcf5960`). Confirms the alignment system's terrain field statically. |
| `0xcf71d0` | AddTile candidate: `endbr64; push rbp; mov rbp,rsp; push r15`; `0xcf71da: 49 89 ff` saves SysV this/rdi in r15, `0xcf71e8: 41 89 f5` saves entity/esi in r13d. Its profiling string at `0x3f25667` is `CTerrain`. |
| `0xcf73f2..0xcf7419` | Loads terrain+0x18 grid, subtracts grid origin, multiplies by width, scales record index by 40, stores entity (`45 89 2c 24`) in record+0. This links the candidate to AddTile independently of its generic profiling label. |
| `0xcf75e4..0xcf7608` | Loads record+0x10 shared ownership control, tests reference count at control+8 against 1, then loads **record+8 directly as the vector**, with last at vector+8 and first at vector+0. |
| `0xcf774a`, `0xcf77ce`, `0xcf77d3` | Detached copy: vector is new control+0x10 (`4d 8d 6e 10`); stores that vector pointer at record+8 (`4d 89 6c 24 08`) and control separately at record+0x10 (`4d 89 74 24 10`). |
| `0xcf7696` | Existing guarded default-append call, vector in rdi and extra sample count in rsi. AddTile increments record version at `0xcf762f`. |
| `0xcf4f78..0xcf4f8d` | Independent detach/read path checks record+0x10 refcount, then reads record+8 and dereferences the vector's first pointer. Corroborates that adding Windows' extra +0x10 here would be wrong. |

The prior native map traversal evidence at `0x173dae0` was rechecked. The
publication call at `0x173e16f` still targets `0xcf56d0`. No assumption that
Windows and Linux shared_ptr layouts agree is made. No hook or byte manifest
entry was added; all 22 existing guarded sites remain verified.

### Not ported: attempts and remaining evidence

**Alignment/sidecar runtime:** the Linux AddTile/vector and terrain-field
investigation above made progress, but the lab could not start (below). Live
private ownership after AddTile, worker completion, both terrain versions'
creation order, save-time ownership and lifetime across loads remain unproven.
The Linux publication/copy paths must be traced before suppressing any writes;
a persistent served flag must not suppress later gameplay edits. The native
batching prerequisite remains absent, including the timing and pass-end callback.
No live system pointer is retained. Save-path resolution and completed-save
triggers also remain absent. In this exact incoming Windows snapshot,
`ArmForLoad` and `WriteForSave` have definitions but **no production callers**;
future Windows wiring is outside this batch. No end-to-end sidecar load claim
is made for either platform here.

**Memory policy:** inspected both Windows workers and Linux's UFFD backend.
Windows uses section/free-commit accounting and a gameplay tick; Linux uses
anonymous MAP_NORESERVE/UFFDIO_COPY, retains a fixed resident target and has no
material pager. The installed menu library's dynamic exports still provide no
`Tpf2mpLastGameUiTick`. This host's overcommit mode is 0, ratio 50; measured
MemTotal/MemAvailable/CommitLimit/Committed_AS are retained in the evidence.
CommitLimit minus Committed_AS is not a validated replacement for Windows free
section commit. The failed live launch prevented loading, frame/fault feedback,
constrained-memory progress and recovery measurements. Importing the formulas
alone would not validate throttle safety. Sized headroom, caps, simulation,
hysteresis, working-set floor and associated material policy stay disabled.
Needed: native pressure inputs, loading/frame signals, recoverable UFFD copy
failures and live constrained-process measurements. No Linux memory or load-time
improvement is claimed.

Explicit `terrain_sidecar=1`, nonzero `terrain_cache_max_mb`,
`material_cache_max_mb` or `simulate_physical_mb` now log native diagnostics.
Tests assert that these requests add no patch sites or writes. Linux defaults
remain minimap=0, sidecar=0 and fixed terrain_cache_hot_mb=1024; compression is
still opt-in. Zero cap/simulation settings do not enable an automatic policy.

### Validation and live attempt

`tools/linux/build.sh`: six suites pass, including the real userfaultfd pager
test (not skipped). `tools/linux/verify_game.py`: build-id and all 22 sites pass.
Host ASan/UBSan sidecar test passes. Windows DLL/serve/alignment tests and the
MSI workflow were not executed on this Linux host.

Backed up both actor share/tpf2mp and mod directories with `cp -a` to
`.before-port`, installed the built plugin and Linux configuration in the
native actor, then ran the official tpf2-multiplayer lab launcher (this clone
has no launcher). It exited immediately with `bwrap: setting up uid map:
Permission denied`. No title menu, renderer/Vulkan device, save load, gameplay
or gdb attachment was reached. No input was sent, save modified, or Steam
process touched. The failure was well within the three-minute time box.
The share directory was restored; both restored share and unchanged mod compare
identically to backups. No lab game process remains. Actor logs/data were
copied for completeness, but pre-existing gameplay logs are not evidence of
this run. Disassembly, launch/build/test logs, memory evidence, process check
and empty restoration diffs are in this job's `meta/live/`.
