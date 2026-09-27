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
It splits temporary names into values at writes that the native code
overwrites completely. Values whose lifetimes do not overlap then share a
name, and an op verified to work element by element may write its output over
an input that dies there. The op semantics, their RVAs and the allocation
behaviour are in `docs/generation-op-semantics.md`. The pass leaves
operation order, parameters, seeds and stock aliases unchanged. It pins names
referenced outside the layer name fields, keeps non-temporary names and values
read before their first full write, and skips unknown layer or op types. The
native ScriptGenerator (`0x399050`) orders layers per buffer name, so sharing
a name serialises otherwise independent branches.

The tested pipelines use these named buffers:

* Desert: 18 -> 15.
* Temperate: 10 -> 9.
* Tropical: 10 -> 10 (the pass is a no-op).

Each result equals the lower bound for these semantics. At 228 x 1140 tiles,
each buffer has 14593 x 72961 floats, so three buffers total 12,776,638,476
bytes (11.90 GiB). This is an expected reduction in named terrain storage,
not a measured reduction of the entire process peak. Additional scratch
allocations and scheduling affect that peak.

## Fantasia Map Generator (low memory)

The Fantasia Map Generator workshop mod (2916150031) builds its pipeline with
the stock `layersutil` temporaries, but no pass runs over it, and it is far
larger: about 16,800 layers over 224 temporary names at 32 x 32 km. The game
MEASURED 59-63 maps (15.8-16.9 GB) at 32 km and 53 maps (22.2 GB) at 40 km,
where it died with `std::bad_alloc` on a machine with no page file.

`python tools/install_fantasia_low_memory.py` installs
`mod/generation/fantasia_low_memory` as the local mod
`mods/tpf2_bigmap_fantasia_low_memory_1`, with a copy of `bigmap_memory.lua`.
Enable it together with Fantasia, below Fantasia in the mod list. Its three gen files have
Fantasia's own file names, so the game uses them in place of Fantasia's. Each
finds Fantasia's `res/` folder through `package.searchpath` on its
`terrain/fmg_mapgenutil` module, runs Fantasia's own gen file in a private
environment, and passes the result through the same pass when the map is
larger than 32 x 32 km (area over 32768^2 m^2). Smaller maps get Fantasia's
pipeline untouched. Fantasia's files are never copied or edited. Loading
prints `[tpf2_bigmap] fantasia_map_generator*.gen.lua: buffer reuse armed`;
if that line is missing, Fantasia's copy won the load order. Without Fantasia
active, the generator is still listed but refuses to generate. `--remove`
uninstalls it.

All three Fantasia climates go from 224-227 names to 10 at 32 km and from
325-328 names to 10 at 40 km, which is the lower bound.
`tools/test_fantasia_low_memory.py` checks name, climate, params, the untouched
32 km pipeline and the symbolic replay at 40 km. Sharing names serialises
layers, so generation may take longer.

## Checks and remaining validation

`tools/test_placement_distance.py` exercises installer refusal paths and runs
the original worker machine code in Unicorn, checking that only the attempt
argument changes and that the remaining native call arguments survive.

`tools/test_generation_memory.py` runs 18 actual shipped Lua pipeline
combinations across Desert, Temperate and Tropical, three water settings and
two seeds (one Desert case uses the 292 km dimensions). It checks:

* unchanged metadata, parameters and pinned names
* stock aliases kept, and new aliases only for in-place-safe ops
* symbolic provenance of every input, and of the old output of every op that
  reads it, using the verified semantics

It also reports before/after counts against a lower bound, and runs guard
cases for unknown schemas, pinned metadata and fresh buffers. Installer backup,
restore, idempotence and conflict refusal are tested in a temporary directory.

These are offline checks, not a native rendered-heightmap comparison or a
live timing/memory benchmark. Measure the same seed and settings after a
restart. Look for the fast-placement startup message and the Lua
`terrain memory: 18 -> 15 named buffers` message, then compare peak private
memory, stage times and resulting town/industry counts. Restore the modes
individually if investigating a difference in results.
