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
Other candidates, including bicubic refinement, alignment scratch reuse and
terrain sidecars, still need native ABI identification and separate tests.

The existing fast-save patches were also tested in the private process after
holding and pausing the world: stock saving took 5.742 seconds, then the
64 KiB / zstd level 1 path took 5.166 seconds. This sequential single-pair
comparison is not a controlled proof of sustained improvement.
The save grew from 117,305,393 to 127,381,563 bytes (about 8.6%). After restoring
all four original save-code sites, the stock loader read the optimized save
successfully and reported `world_ready` in 57.863 seconds.
