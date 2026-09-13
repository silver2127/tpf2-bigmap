# Experimental generation performance modes

These changes preserve depth 13, 128 m octree leaves, map dimensions and the
4 m heightmap resolution. Restart before measuring a new preview.

## Fast placement

`placement_attempts=50` in the active `plugins/tpf2_bigmap.cfg` reduces the
RandomLocationFactory worker's inner optimization budget from 200 to 50.
Set 200 to restore stock behavior; accepted values are 1 through 200.
The source config defaults to 200; the experimental local deployment uses 50.

Steam 35924 RVA `0x912f59` is `41 b9 c8 00 00 00` (mov r9d,200) in the
worker at `0x912f10`. Only this verified immediate is changed. The worker
calls `0x910f40` with this fourth argument. Four workers, the outer passes,
minimum spacing, water/slope/obstruction checks and requested counts remain
unchanged. Fewer attempts can yield less even placement and fewer accepted
sites. This is a 75% reduction in inner attempts, not a measured 4x speedup
for total generation. Unknown bytes/builds and invalid values are refused.

## Terrain buffer reuse

Run `python tools/install_generation_memory.py` to enable the Lua pass for
the three stock New Game terrain generators. Exact originals are backed up
as `*.gen.lua.bigmap-memory.bak`. Use
`python tools/install_generation_memory.py --restore` to restore those files.
The installer preflights all three files and refuses manual edits conflicting
with a backup. It does not overwrite a differing existing helper module.

`mod/generation/bigmap_memory.lua` examines the completed Lua operation list.
It aliases temporary buffers whose entire named lifetimes do not overlap,
and only when the later buffer's first operation is a distinct-input MAP
that overwrites its full contents. It leaves all other operation parameters,
seeds, order and aliases within an operation unchanged. Names referenced by
output metadata are pinned. Unknown schemas are skipped. The native
ScriptGenerator (`0x399050`) builds dependencies from buffer accesses;
sharing a name adds ordering between otherwise independent branches.

This deliberately avoids broader assumptions about feature operations that
may accumulate into their existing output or depend on zero initialization.
The current tested Desert pipelines use 16 named buffers instead of 18.
At 228 x 1140 tiles, each buffer has 14593 x 72961 floats; two buffers total
8,517,758,984 bytes (7.93 GiB). This is an expected reduction in named terrain
storage, not a measured reduction of the entire process peak. Additional
scratch allocations and scheduling affect that peak. Tested Temperate and
Tropical pipelines remain at 10 buffers; the pass is a no-op for them.

## Checks and remaining validation

`tools/test_placement_distance.py` exercises installer refusal paths and runs
the original worker machine code in Unicorn, checking that only the attempt
argument changes and that the remaining native call arguments survive.

`tools/test_generation_memory.py` runs 18 actual shipped Lua pipeline
combinations across Desert, Temperate and Tropical, three water settings and
two seeds (one Desert case uses the 292 km dimensions). It checks unchanged
metadata and parameters, no overlapping aliased lifetimes, identical aliases
inside operations, and symbolic input/output provenance. Installer backup,
restore, idempotence and conflict refusal are tested in a temporary directory.

These are offline checks, not a native rendered-heightmap comparison or a
live timing/memory benchmark. Measure the same seed and settings after a
restart. Look for the fast-placement startup message and the Lua
`terrain memory: 18 -> 16 named buffers` message, then compare peak private
memory, stage times and resulting town/industry counts. Restore the modes
individually if investigating a difference in results.
