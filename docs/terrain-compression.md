# Lossless 1 m terrain compression (Steam 35924)

Implemented September 13, 2026. Version 2 is built and installed; native tests,
world loading, terrain inspection and two connected rail builds passed.
The resulting save completed in 13.686 seconds and reloaded successfully with
both rail segments intact. Extended gameplay remains untested.
This is separate from the discontinued 2 m experiment. Source heightmaps,
derived 257x257 samples, physical tile dimensions and octree depth are unchanged.

Runtime check, PID 83640 on September 13: the user reported 16 GB after loading;
read-only process inspection measured 16.30 GiB working set and 18.46 GiB private
commit. Logs confirm 1 m restoration, compression enabled and 64,980 live tile
versions, with failures=0 throughout the inspected interval. Several samples
settled at 1,023.9 MiB resident backing plus about 2,207..2,235 MiB cold commit
(roughly 3.2 GiB combined, versus 7.994 GiB stock payload). Other samples showed
larger active sets and temporarily 129,960 live versions; their cause has not
been correlated with a user operation. Fault/eviction counts rise substantially
during activity. This confirms compression and lower backing use, not an
end-to-end loading-speed result or completed gameplay correctness test.

## Version 2 runtime check (September 13)

Installed DLL SHA256:
`F55A8CAB908BAB48C34D88DC714BB9B904224EE8FDE036BF953FCB87A8BE5282`.
PID 67936 loaded the preserved current 114x570-tile desert world. Settled
terrain backing was 1,023.9 MiB resident plus 1,582.1 MiB encoded commit,
about 2.545 GiB combined, versus about 3.2 GiB for version 1. The codec's
compressed payload was about 1,446 MiB. No pager failures were logged.

The loaded process measured 14.44 GiB working set and 16.71 GiB private commit
before rail construction. Version 1 measured 16.30/18.46 GiB, but that was a
newly generated session rather than this saved reload: the entire difference
must not be attributed to version 2. A load sample reached 32.81 GiB working
set while two terrain versions existed; startup peaks remain substantial.

Terrain looked continuous at several camera heights. Two connected electrified
rail segments (220 m straight, 467 m curve) built west of Augusta without a
crash. Shared compressed clones and write-fault counters advanced normally;
failures stayed zero. The separate `n.sav` output was copied, with metadata and
preview, to `MemoryV2-RailTest-20260913`; the original current world remains
preserved as `MemoryBaseline-20260913` (also `m.sav`). No original user saves
were overwritten.

The test save reloaded in the same process and reached the world by 02:28:50.
Both segments were present, terrain remained continuous, and simulation ran
before being paused. A post-reload sample measured 14.78 GiB working set and
16.87 GiB private commit; active backing was still settling. The inspected
pager counters remained at failures=0 after both loads, construction and save.
Test-save SHA256:
`141658BE3C235984D1964A98F2DA1CEF963BCA96FA5DC4A677E575DC5989F08B`.

The temporary 4 GiB budget was observed during bulk allocation, followed by
the normal 1 GiB budget. This heuristic does not cover every loading stage.
Repeat evictions reused compressed blobs hundreds of thousands of times.
These are mechanism checks, not a measured end-to-end loading speedup.

## Configuration

```
terrain_cache_spacing_m=1
terrain_cache_compress=1
terrain_cache_hot_mb=1024
terrain_cache_warm_mb=4096
```

Restart and load a world to enable. To disable, set `terrain_cache_compress=0`
and restart. Compression is a runtime allocation policy, not a save format.
Source configuration defaults to disabled. The resident target accepts
128..8192 MiB. Compressed storage is additional; recent or incompressible tiles
can temporarily exceed the resident target. The pool can manage 131,072 live
tile versions; excess allocations use the stock heap.

The optional warm target accepts 0..8192 MiB. The latest build keeps it during
instrumented new-world entry and through late loading after bulk allocation
(at least 1,024 slots in a one-second allocation window), only while at least
8 GiB RAM is available. Set warm_mb=0 to disable.

The matching new `tpf2_menu.dll` exports `Tpf2mpLastGameUiTick`, captured by its
existing CGameUI update hook. After the last generation/allocation activity,
the pager requires a fresh gameplay UI frame and at least five seconds of
grace before releasing the loading allowance. Paused gameplay still updates
the UI. Stale ticks from the previous world cannot end a subsequent load's
allowance. A cancelled/stalled load has a 15-minute tail cap. If the menu DLL
does not expose the signal, the fallback is a bounded three-minute tail.
Neither timeout expires while instrumented generation remains active.

This follow-up is built and passes native policy tests, but is not installed
over the currently running process. Its actual loading speed remains unmeasured.
The prior runtime results above are for the earlier 15-second-tail build.

## Ownership and synchronization

`terrain_compression.h` intercepts the uint16 vector resize at `1d5c50` only
for the `CTerrain::AddTile` call returning to `33ccaa`, an empty vector, and
66,049 samples. It intercepts the COW copy constructor at `1dedd0` only for
the `33dd20` call returning to `33dd91`, and the shared control block's vector
destructor at `33de30`. All prologues and both call instructions are verified
before hooks install. Managed allocation starts only after all hooks and the
worker succeed; destruction installs first. Unexpected vector growth migrates
back to the game allocator. Existing allocations are never retrofitted.

`terrain_pager.h` reserves a placeholder arena with a fixed 135,168-byte slot
per tile version (132,098 data bytes, 32-byte aligned-allocation header, page
rounding). Resident data uses a pagefile-backed section. To evict a tile:

1. Lock the pool and map a private read alias of its section.
2. Protect the game-facing view as inaccessible. Concurrent engine accesses
   now fault and wait; completed writes are included in the snapshot.
3. Encode row differences modulo 65536, zigzag signed deltas, pack four nibble
   planes (two samples per byte), compress with
   LZ4 1.10.0, and copy the result into ordinary committed virtual memory.
4. Unmap the public view back to its placeholder, unmap the alias, and close
   the section. This releases backing commitment; it is not a working-set trim.
5. Release the lock. A fault decompresses into a new private alias and maps the
   completed section into the original address before allowing the instruction
   to retry. Both reads and writes resume against unchanged addresses.

Version 2 retains immutable compressed data after read restoration and maps
that resident view read-only. Re-eviction reuses the representation without
encoding again. First write remaps the same section at the same address with
read/write permission, then drops that owner's compressed reference. Windows
cannot upgrade a read-only section view with VirtualProtect alone; the native
tests caught this and exercise the remapping transition directly.

COW copies can share a reference-counted immutable compressed representation
without restoring either tile. Each version receives its own fixed address and
private section on restoration. Parent destruction and writes to one version
do not invalidate other versions. Compressed commitment counts unique blobs.

Compressed bytes use VirtualAlloc/VirtualFree rather than the game heap, so
world teardown releases them directly. Fault handling allocates no STL objects,
calls no engine code, does not log, and restores the interrupted last-error
value. It handles only read/write access violations inside active owned slots;
unrelated exceptions pass through. The private alias avoids snapshot races with
engine readers/writers without suspending game threads. Failed eviction keeps
the intact original section. A genuine restore allocation failure propagates
the access violation instead of inventing terrain data.

The worker scans up to 256 slots per 25 ms tick, evicting above-budget resident
tiles at least five seconds old. Age is allocation/last-restoration time, not
an exact access LRU. Frequently accessed resident tiles are not instrumented.
This may cause unnecessary restores under heavy load; logs expose faults and
evictions so that policy can be tuned. Pool transitions currently serialize.

## Measured tests

`build.bat -pager-test` builds the standalone Windows test executable. With the
1,024-tile CHONKYIe sample (seed 20260913, SHA256 in the earlier JSON benchmark):

- V2 LZ4 payload ratio: 0.180614, projected 1.444 GiB for 64,980 tiles.
- Cold commitment including allocation header/4 KiB rounding: projected 1.578 GiB.
- V1 on the same samples: 2.373 GiB payload, 2.497 GiB rounded commitment.
- Allocation/copy/eviction/fault restoration/verification/destruction of all
  1,024 samples took 578 ms in one V2 run while the game was running.
- Every sample round-tripped exactly. 4,000,000 concurrent atomic writes raced
  actual page protection and section eviction without lost/duplicated writes.
- 4,096 simultaneous tile allocations were compressed/restored/destroyed;
  handle counts returned to baseline and all pool backing counters to zero.
- Write faults, address reuse with zero initialization, incompressible fallback,
  COW isolation, compressed destruction, resize migration, installer failures,
  original Steam hook bytes and instruction boundaries passed.
- Read-only restoration followed immediately by a write, 32 shared clones with
  parent released first, and eight concurrent clone/read/write/destroy workers
  racing eviction passed. Unchanged re-eviction performed no new encode.
- Five codec variants were compared by `build.bat -codec-bench`. Nibble planes
  preserved the old encoder's approximate CPU cost while improving space;
  bit planes saved more but more than doubled encode/decode work and were rejected.

With a 1 GiB resident target, V2 sample extrapolation suggests roughly 2.6 GiB of
cache backing including retained representations versus 8 GiB stock, before
other overhead and transient versions.
This is **not** a measured reduction in the running game. Mapped section bytes
must be counted alongside private commitment; a lower process PrivateUsage
number alone would not prove a physical-RAM reduction.

## First gameplay validation

Load an existing original 1 m save, observe compression logs (30-second cadence),
check terrain across tile boundaries, pan/zoom, preview and build rails/roads,
terraform, undo where supported, save to a separate test name, then reload it.
Check the `failures` counter, load duration, frame stalls and total physical
memory as well as resident/cold backing counters. Existing original saves
should be retained. Kernel APIs accessing a cold user buffer directly do not
necessarily deliver a resumable user-mode fault; known height-cache consumers
use CPU loads/copies, but gameplay and serialization integration must verify
that assumption. Source terrain heightmaps are not managed by this pager.

## Dependencies and API references

Vendored [LZ4 1.10.0](https://github.com/lz4/lz4/tree/v1.10.0/lib), BSD 2-clause
license retained in `src/vendor/lz4/LICENSE`; files downloaded from that tag.
The mapping protocol uses Microsoft's documented
[placeholder allocation](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2),
[placeholder replacement](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3),
and [placeholder-preserving unmap](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-unmapviewoffile2).
These require Windows 10 version 1803 or later; dynamically resolved APIs make
unsupported systems refuse this feature before allocating any game tiles.
