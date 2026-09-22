# Completed terrain-block cache (experimental)

Native Steam 35924; `terrain_chunk_cache=0` by default.

Unlike `terrain_kernel_cache`, this caches the completed output of the terrain
alignment worker, including refinement and alignment, in resident memory. It
does not open a file for each refinement/alignment call. Entries survive world
reloads in the same game process and do not survive process exit. This is not a
Windows `.terr` reader or a persistent save-sidecar implementation.

## Contract

The worker at `0x173b6e0` takes a context and a half-open range of 88-byte work
records. Context +0 points to the alignment system (terrain at system +8), +8
holds scale/offset, and +16 points to the work-record vector. The worker's only
published output is each record's `vector<uint16_t>` at +0x40. On a hit the
engine's own append function grows that vector, and all 65x65 samples are
restored. The engine's subsequent publication/minmax work runs normally.

Supported records have base/high terrain levels 6/8, 65x65 output, a 64x64 local
extent aligned to four samples and bounded by 0..256. Other shapes execute the
original worker. Original worker/sampler/wrapper bytes are guarded; no Windows
calling convention or object layout is reused.

Keys contain:

- Native build and cache schema; floating-point control bits.
- Terrain levels/scales, worker scale/offset, tile coordinates, absolute box
  and local sample rectangle.
- Every ordered alignment's type, triangle bytes and weight bytes, with lengths.
- The 19x19 source rectangle, including the interpolation halo. The original
  engine base sampler (`0xcf55a0`) supplies its exact boundary handling.

Full keys are compared after hashing. Pointer identity, save filename and world
identity do not determine hits. Changed base heights or alignment inputs miss;
unchanged blocks can be reused after a renamed save or a different snapshot.

## Bounds and verification

`terrain_chunk_cache_mb` bounds charged resident entry storage, including an
allowance for container/allocator overhead (64..4096 MiB, default 1024). Scratch
and game output allocations are separate. Entries are sharded under mutexes;
when full, existing hits work and new entries are omitted. There is no eviction
policy yet. No game pointer is retained in an entry.

`terrain_chunk_verify=1` is the default when the experiment is enabled. Every
hit still runs the original worker and compares the entire output. A mismatch
poisons the cache for the remainder of the process; original results remain in
the game. `terrain_chunk_verify=0` permits verified cache entries to skip work
and is for controlled benchmarks until game acceptance is complete.

Counters in `tpf2mp_host.log` report calls, hits, misses, stored entries, charged
bytes, refusals, verified hits, mismatches and budget refusals. Unit checks cover
input changes, floating controls, bounded/concurrent storage, worker hits,
allocation growth and verification. Real game comparison results are recorded
separately; synthetic tests alone do not prove engine compatibility.

## VPS verification, September 22

The 1 GiB verification run loaded the private test world and saved/reloaded a
117,305,246-byte checkpoint. It compared 305,340 complete blocks (1,290,061,500
height samples) against freshly executed native calculations: zero mismatches
and zero unsupported records. There were 836,352 observed worker records;
61,068 distinct cached blocks filled the budget. The cache never replaced a
calculation in this run. Seven Soldier CTest suites passed.

Verification reloads took 79.752 and 86.801 seconds, with 75.406 seconds when
the chunk hook was temporarily restored to the original worker between them.
The previously deployed fast kernels remained enabled throughout. These are
verification overhead measurements, not cache speedup measurements.

This private run's startup took 271.38 seconds, including a slow later loading
stage and a brief sampling profile. Do not compare it directly with previous
startup measurements or attribute the whole delay to terrain computation.

With a 3 GiB limit and verification disabled, the complete map fit: 139,392
entries, 2,467,104,168 charged bytes (about 2.30 GiB), no budget refusals. Both
terrain versions and subsequent reloads reused the completed blocks. A fresh
117,305,465-byte checkpoint saved in 3.855 seconds. Same-process comparisons:

- Cache enabled, first reload: 59.511 seconds.
- Original worker restored (other fast kernels still enabled): 66.252 seconds.
- Cache enabled, repeat reload: 73.065 seconds.

All reached `world_ready` and accepted hold/pause. The two cached runs average
66.288 seconds, effectively the same as the control. The first improvement did
not repeat, so these measurements do not establish a whole-load speedup. The
experiment stays OFF in production and the performance preset. There is no
claim of a measured client-join improvement. No new production restart was
performed for this experiment; the current live world and Steam offline state
were left intact.

The native plugin diagnostics thread was subsequently moved after successful
transactional patch installation. The worker/cache code is unchanged; the final
build again passed all seven CTests.

The final 3 GiB verification startup then checked all 139,392 distinct cached
blocks against the second terrain version: zero mismatches, zero unsupported
records and no budget refusals. This adds 588,931,200 sample comparisons and
covers the complete test map, rather than only the first 1 GiB of entries.
Final experimental plugin SHA-256:
`ff8f859b6921f9339f29076593d0391e5538091b09e881eef0ac0df99c64a465`.
Production retains the earlier plugin:
`51c2893b7e9001a44721af9caa4f43b01d17ee79ca5dcfc6b99e17fcfd881b95`.
