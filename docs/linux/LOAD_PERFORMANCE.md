# Native terrain loading experiments, September 22, 2026

Target: native Steam build 35924, ELF build ID
`3a0e156390b0e6f1e372051c24802c8493ae454a`.

The native multiplayer VPS profile sampled two terrain loops prominently:
the min/max scan at `0xcf586c..0xcf5883`, and the uint16 row copy at
`0xdb6b1c..0xdb6b20`. The existing Linux min/max implementation was available
in this repository but was not installed on that server.

## Row-copy port

The Windows `terrain_minmax_fast` feature includes a faster height-block copy.
Linux inlines the corresponding row loop into the terrain publication walk.
`terrain_copy_fast=1` now replaces exactly `0xdb6b10..0xdb6b29` after checking
all 25 original bytes. The original outer loop still chooses regions and
advances the source/destination rows.

At entry RSI is the source, RCX the destination, and RDI a positive even byte
count. At exit RAX equals the count and EDX contains the final uint16 read.
The bridge preserves all other registers and the stack. Subsequent stock
instructions overwrite flags before consuming them. As required by the SysV
ABI, the direction flag is clear.

Disjoint rows use `rep movsq` plus a uint16 tail, without reading beyond the
row. Every overlapping row uses the original forward uint16 loop. This
includes identical pointers and odd-byte overlap: replacing those cases with
`memmove` would change behavior. Row order remains unchanged even when one
row overwrites a later row's source.

The bridge contains no calls, allocations, terrain precision changes, floating
point operations, or changes to simulation data structures. It is opt-in.

## Validation

- Soldier SDK: five CTest suites pass.
- The copy test executes the original relocated machine code and compares the
  complete backing buffer and nine output registers against the replacement.
  Cases include tiny/tile/large rows, both overlap directions, identical
  pointers, odd offsets, and disjoint spans.
- Protected-page tests place the exact source and destination ends against an
  inaccessible page, checking 257 row lengths for overreads/overwrites.
- Plugin tests cover opt-in installation, rejection on a copy-site byte
  mismatch before mutation, rollback, and performance-only patch scope.
- The private VPS game successfully installed the guarded patches and loaded
  the same production checkpoint with each configuration.

## Server configuration

Put `tpf2_bigmap.so` and `tpf2_bigmap.cfg` beside each other in the native
multiplayer installation's `plugins` directory. An experimental configuration:

```ini
[tpf2_bigmap]
enabled=1
performance_only=1
terrain_minmax_fast=1
terrain_copy_fast=1
save_fast=0
terrain_cache_compress=0
```

`performance_only=1` skips map-menu, density, street raster and octree hooks,
including density-file restoration. Min/max and save optimizations default
off in this mode and must be selected explicitly. Terrain compression remains
a separate opt-in setting. Restart the game to apply or remove patches.

## Initial load measurements

Fresh private processes loaded the same `mp_offline0922` checkpoint with the
same settings and copied shader cache while the production game stayed active.
Times are process launch to the native controller reporting a loaded world;
they include the dedicated startup delay. The profiler was off for timing.

- No native Big Maps plugin: 119.12 seconds.
- Existing min/max optimization: 114.10 seconds.
- Min/max plus new row copy: 116.10 seconds.
- Repeat with performance-only initialization: 114.09 seconds.

These single runs do not establish a meaningful whole-load improvement. VPS
contention and filesystem/cache variation remain uncontrolled. The row-copy
switch stays off by default, and production was not restarted for these tests.
The earlier 240 FPS experiment took 121.11 seconds and also showed no benefit.

Do not extrapolate the Windows microbenchmark speedups to whole Linux loads.
The follow-up below implements native refinement, alignment and calculation
caching; these were not included in the initial measurements above.

The existing fast-save patches were also tested in the private process after
holding and pausing the world: stock saving took 5.742 seconds, then the
64 KiB / zstd level 1 path took 5.166 seconds. This sequential single-pair
comparison is not a controlled proof of sustained improvement.
The save grew from 117,305,393 to 127,381,563 bytes (about 8.6%). After restoring
all four original save-code sites, the stock loader read the optimized save
successfully and reported `world_ready` in 57.863 seconds.

## Follow-up: exact native terrain kernels

`terrain_refine_fast=1` replaces native `InternBicubicRefine` at `0xd9b850`.
Its CVec3f argument is a SysV aggregate in xmm0/xmm1; the Windows pointer ABI
must not be used. The complete 0xb3d-byte function has FNV-1a fingerprint
`b1513c7c66b75843`; its first 16 position-independent bytes form the trampoline.
The SSE2 implementation retains operation ordering and runs stock code for
unsupported factors/assert cases. FMA contraction is disabled at compile time.

`terrain_align_fast=1` replaces `CalculateHeightMod` at `0xda1a70` (0xc76 bytes,
FNV-1a `24620013235767ef`). The first 15 bytes form its trampoline. The port
uses pooled scratch, exact SSE2 blending, the native rasterizer at `0x31bd660`
and triangle entry at `0x31bd6e0`. Triangle coordinates are three SysV SSE
aggregates. An empty alignment list leaves output unchanged and bypasses
scratch fills and scanning. A scope guard returns scratch if rasterization
throws. Unsupported inputs use the original implementation.

Both patches participate in the plugin's preflight and rollback plan. The
host must accept the exact game build before either function is read/patched.

Tests execute the original functions from a private image of the user's ELF:

- Refinement: 432 comparisons, 359,765,664 samples, including overlap cases.
- Alignment: 1,536 comparisons, 16,957,824 samples, including randomized
  triangles, all three types, default/custom weights, empty lists and edges.
  Cases include the live 65x65 block shape and eight scales/four offsets.
- Cold and warm cached outputs are compared with the same original outputs.
- Six Soldier CTest suites pass, including corruption, wrong-input/size,
  concurrent publication and budget tests for the persistent cache.

The oracle executables accept the native `TransportFever2` path. They are
standalone tests, not live-process injection. The game image is not distributed.

## Persistent calculation cache

`terrain_kernel_cache=1` enables a native cache under the mod data directory,
`terrain-kernels-v2`. `terrain_kernel_cache_mb` bounds new entries (default
2048 MiB). Restart after changing settings. Missing, malformed, foreign,
truncated or checksum-invalid entries fall back to computation. Full keys are
compared, so filename hash collisions cannot serve another input's output.
Files are published atomically and cache allocation/I/O failures are optional.
Once the budget is full, valid hits remain available and new writes stop.

This replaces the proposed direct Windows `.terr` port with exact-input
calculation reuse. It does not read/write Windows sidecars. Refinement keys
include every source sample read, call parameters and floating-point controls;
alignment keys also include the original result, triangle lists and weights.
Save renaming or copying has no effect on matching. Edits change the inputs.
Aliased calls bypass the cache. Hits skip calculation and restore only the
function's output region. Empty alignment calls bypass cache I/O entirely.

Disk caching is **off by default**. The initial implementation took 154.15 s
cold and 135.30 s warm, versus 90.11 s with only the fast kernels and a
15-second menu grace. It encountered 557,568 calls and filled its 2 GiB budget.
This motivated word-wise checksums and skipping empty alignment cache entries.
Do not enable disk caching based solely on its hit count.

The kernel-only follow-up repeated at 92.12 and 93.11 s. These are whole startup times,
including a separately configurable native multiplayer startup grace. They
are not isolated kernel speedup measurements; the VPS also runs production.

The revised cache measured 151.23 s cold and 118.19 s warm. It remains slower
than computation on this save and stays disabled in the server configuration.
The cache implementation is experimental; these results do not justify
enabling it in production. A separate SDL startup failure before any terrain
calls was discarded; the benchmark harness now waits for the private display
and stops the game before stopping Xvfb.

The remaining presentation experiment (`dedicated_nowsi=1`) took 94.10 s with
the kernels and 15-second grace, versus 92.12 s normally. No new graphics skip
is enabled. Terrain tessellation/SSAO/shadows were already disabled. Skipping
render-data initialization wholesale still needs proof that its CPU consumers
do not require the resulting vectors.

## In-session save/reload check

The private server held and paused the loaded world, saved `mp_perfcheck`
(117,305,444 bytes, 4.271 s), then loaded that exact file three times in the
same process. For the middle load only, the refinement/alignment prologues
were restored to stock; min/max and row-copy stayed enabled throughout.
The two detours were restored before the last load and again in `finally`.

- New kernels, first reload: 69.484 s.
- Stock refinement/alignment: 73.711 s.
- New kernels, repeat reload: 72.371 s.

All three emitted successful `world_ready` and accepted pause/hold commands.
This is only a modest observed improvement (1.3–4.2 seconds), subject to cache
and VPS scheduling variation. It does not establish a large join-time gain.
The 15-second startup grace does not apply to these reloads.

Standalone native kernel measurements (same VPS, original ELF functions):
10,000 19x19 refinement calls took 0.273316 s stock / 0.103551 s optimized;
1,000 65x65 alignment calls with six triangles took 0.111373 / 0.055869 s.
Empty alignment calls took 0.015595 / 0.000294 s (including output reset).
These microbenchmarks explain local wins, not a proportional whole-load gain.
