# Terrain sidecar: reuse a save's aligned 1 m cache on load

The load-time terrain alignment pass (`docs/alignment-batch.md`) rebuilds the
1 m height cache every load: the bicubic refine turns the 4 m base heightmap
into a 1 m base cache, then the alignment pass cuts every road, track and
construction into it. Batching (`alignment_batch_tiles`) removed its memory
spike (35 GiB -> 8.3 GiB on a 207,360-tile save), so what remains is its
**time**. The finished cache is a pure function of the save, so writing it
beside the `.sav` and restoring it on load of the *same* save skips both the
refine and the pass.

## Status

Built and tested here: the **file format**, the **grid walk** that reads and
restores every tile's height cache, and the **codec** (the variable-length
`BlockCodec` from `small_codec.h`). `tools/test_terrain_sidecar.cpp` builds a
synthetic grid to the game's exact layout and checks a full round trip, plus
every refusal path. Measured on synthetic terrain-like tiles: **17.6 % of raw**,
about 22 KB per 257x257 tile, so a full 207,360-tile map is roughly **1.2 GiB**
on disk.

Not yet integrated (needs the pass, which `src/alignment_batch.h` owns):

1. **Fingerprint.** Key the file to the save by a 64-bit hash of the `.sav`
   bytes, captured in a `SaveGame` (`0x2e97c0`) post-hook and recomputed in a
   `LoadGame` (`0x2e5ec0`) pre-hook. Same bytes => same aligned terrain, so a
   match is exact; any mismatch (edited save, plugin-less save, or a *different
   save of the same map* — same base heightmap, different roads) makes `Apply`
   a no-op and the pass runs. A content hash of the base heightmap alone is
   NOT enough: it would false-accept a different save of the same map and serve
   the wrong cuts, so the key must be tied to the save's identity.

   The save's resolved path is reachable exactly as the multiplayer menu DLL's
   `AutoLoadCall` does it (native/src/menu_hook.cpp ~2219): the argument is a
   `SaveGameId { std::wstring path@0x00, name@0x20, namespace@0x40 }` (SSO);
   `app = APP_ACCESSOR()`, `mgr = *(app + 200)` is the save manager, and
   `SAVEINFO_GET(info, mgr, id)` fills a 0x110-byte save-info struct that
   carries the absolute `.sav` path. `LoadGame` (`0x2e5ec0`) receives that
   SaveGameId directly; `SaveGame` (`0x2e97c0`) has it too. Hash the resolved
   `.sav` file and place/read `<path>.terr` beside it. Store the hash in
   `static uint64_t TerrainSidecar::g_saveFingerprint`, which `Write` and
   `BeginApply` already take as their `fingerprint` argument.
2. **Write trigger.** After a save completes, walk `g_alignmentTerrain`
   (exposed by the batch detour, `CTerrain` from `self+8`) and write the file.
3. **Read trigger and short-circuit.** The pass CREATES the tiles (`AddTile`,
   `0x33cb60`, is called from the alignment system), so the sidecar cannot
   replace it wholesale. Two options, to decide with the pass owner:
   - Let the pass create the tiles but make its per-block compute a no-op when
     a valid sidecar is loaded, then `Apply` the saved caches. Smallest change,
     but it lives inside the pass.
   - Serve the saved cache from the refine detour and skip the rasterise. This
     is what "the sidecar replaces what the refine produces" would mean, but
     the refine yields *base* heights and the pass cuts them afterwards, so it
     only works if the cut is also suppressed for served tiles.

## File format

Little-endian. `TERR`, version 1.

```
FileHeader { u32 magic; u32 version; u64 fingerprint; i32 nx, ny; u32 tiles; u64 headerHash; }
repeated:  TileHeader { u32 recordIndex; u32 bytes; }  then `bytes` of BlockCodec blob
```

`headerHash` catches a torn write; each tile's `BlockCodec` blob carries its own
64-bit content hash, so a flipped byte fails decode rather than restoring wrong
terrain. `Apply` refuses the file unless `magic`, `version`, `headerHash`,
`fingerprint` and `nx`/`ny` all match, only writes into a record whose vector is
already `Samples` long (so it never allocates a tile), and returns -1 on a
corrupt-but-matching file so the caller discards it and lets the pass run.

## Grid layout (RE, Steam 35924)

`CTerrain + 0x18` -> grid `{ i32 x0@0, i32 y0@4, i32 nx@8, i32 ny@0xc, void* records@0x10 }`;
each record is 40 bytes `{ i32 entity@0, {u16* first, *last, *end}@8, i32 version@0x20 }`.
Confirmed from `CTerrain::GetTile` (`0x33d580`) and `AddTile` (`0x33cb60`).
