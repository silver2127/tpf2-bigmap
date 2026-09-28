// The terrain sidecar on the native Linux build 35924 (2026-09-28): write
// "<save>.terr" beside every save, and on the load of that same save restore
// every tile from it and skip the alignment pass. Same file format as Windows
// (../src/terrain_sidecar.h), so a sidecar written on one serves on the other.
//
// Simpler than the Windows path on purpose: the sidecar is used only when it
// holds EVERY tile of the terrain. Then no publication of the pass is needed at
// all and the pass is not called; its one other effect, each tile record's
// minZ/maxZ/version (publication 0xcf56d0: 0xcf58ad/0xcf58b4/0xcf58bb, scale
// at CTerrain+0x34, 0xcf5809), is written from the range noted at AddTile. A
// partial sidecar is applied at AddTile and then overwritten by the stock pass
// -- the same result as without it. There is no per-copy skip to get wrong.
//
// Sites (docs/linux/PORT.md, "Terrain sidecar"):
//   SaveGame 0xc7ec00  (meta, shot, cfg, res, state, gui, SaveGameId* on the
//                       stack, bool, monitor); std::optional<SaveGameError> is
//                       returned in rax:rdx. The id's path must be empty (a
//                       non-empty one asserts, 0xc7ec5c); its name is at +0x20.
//   LoadGame 0xc7ca40  (hidden return, ctx, modRep, SaveGameId* in rcx, ...).
//   AddTile  0xcf71d0  (CTerrain*, int entity): grid at +0x18, 40-byte records,
//                       index (x-x0)+(y-y0)*nx (0xcf73f2).
// The save folder is <local>/save, <local> being TPF2MP_USERDATA or the folder
// of the game's own open crash_dump/stdout.txt (the menu's rule, SAVE-02).
#pragma once
#include "../src/terrain_sidecar.h"
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace linux_sidecar {
using LogFn = void (*)(const char*, ...);

struct LStr { const char* p; size_t n; char buf[16]; };   // libstdc++ std::string
struct SaveGameId { LStr path; LStr name; LStr ns; };
static_assert(sizeof(SaveGameId) == 0x60, "platform::SaveGameId");

// ---- the save folder ----------------------------------------------------------
inline std::string LocalFromOpenLog() {
    const char* suffix = "/crash_dump/stdout.txt";
    for (int fd = 0; fd < 4096; ++fd) {
        char link[64], target[1024];
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        const ssize_t n = readlink(link, target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = 0;
        const size_t len = size_t(n), sl = strlen(suffix);
        if (len > sl && !strcmp(target + len - sl, suffix)) return std::string(target, len - sl);
    }
    return std::string();
}
inline std::string SaveDir() {
    static std::mutex m; static std::string cached;
    std::lock_guard<std::mutex> lock(m);
    if (!cached.empty()) return cached;
    std::string local;
    const char* env = getenv("TPF2MP_USERDATA");
    struct stat st{};
    if (env && env[0] == '/' && !stat(env, &st) && S_ISDIR(st.st_mode)) local = env;
    else local = LocalFromOpenLog();
    while (local.size() > 1 && local.back() == '/') local.pop_back();
    if (!local.empty()) cached = local + "/save";
    return cached;
}
// <save folder>/<name>.sav for a SaveGameId, or empty.
inline std::string SavPath(const SaveGameId* id) {
    if (!id || id->path.n || !id->name.p || !id->name.n || id->name.n > 400) return std::string();
    const std::string name(id->name.p, id->name.n);
    if (name.find('/') != std::string::npos) return std::string();
    const std::string dir = SaveDir();
    return dir.empty() ? std::string() : dir + "/" + name + ".sav";
}
inline bool Mtime(const std::string& p, struct timespec* out) {
    struct stat st{};
    if (stat(p.c_str(), &st)) return false;
    *out = st.st_mtim; return true;
}

// ---- the load: what AddTile applied, and each tile's height range ------------
// A load builds two CTerrain versions (Windows served 68,086 tiles of a
// 36,992-tile map), each with its own grid, so the ranges are kept per grid.
struct Served {
    // finished: this version's pass was skipped once; a later big pass on it must run.
    struct Grid { uint8_t* base = nullptr; std::vector<uint32_t> range; uint32_t applied = 0; bool finished = false; };
    std::mutex m;
    Grid grids[4];
    static constexpr uint32_t kNone = 0x0000FFFFu;   // lo | hi << 16 with lo > hi: never a real range
    void Reset() { std::lock_guard<std::mutex> l(m); for (auto& g : grids) g = Grid{}; }
    // Caller holds m. The slot for `base` (n records), made if new; nullptr when all four are taken.
    Grid* Slot(uint8_t* base, uint32_t n, bool make) {
        for (auto& g : grids) if (g.base == base && g.range.size() == n) return &g;
        if (!make) return nullptr;
        for (auto& g : grids) if (!g.base) { g.base = base; g.range.assign(n, kNone); g.applied = 0; g.finished = false; return &g; }
        return nullptr;
    }
};
inline Served& S() { static Served* s = new Served; return *s; }

inline uint32_t HeightRange(const uint16_t* h) {
    unsigned lo = h[0], hi = h[0];
    for (size_t i = 1; i < TerrainSidecar::Samples; ++i) { const unsigned v = h[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    return lo | (hi << 16);
}
inline void Note(const TerrainSidecar::Grid& g, uint32_t idx, const TerrainSidecar::TileVector* v) {
    const uint32_t n = uint32_t(g.nx()) * uint32_t(g.ny());
    const uint32_t r = HeightRange(v->first);
    Served& s = S(); std::lock_guard<std::mutex> l(s.m);
    Served::Grid* slot = s.Slot(g.base, n, true);
    if (slot && idx < n && slot->range[idx] == Served::kNone) { slot->range[idx] = r; ++slot->applied; }
}
// True -- every record's minZ/maxZ written and version bumped as publication
// does -- when every record of `terrain` holds a full tile the sidecar applied.
// False, changing nothing, otherwise. Only while the sidecar is loaded (the
// load's own pass).
inline bool AllServedFinish(void* terrain) {
    if (!terrain || !TerrainSidecar::Loaded()) return false;
    const TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
    if (!g.base || !g.records() || g.nx() <= 0 || g.ny() <= 0) return false;
    const uint32_t n = uint32_t(g.nx()) * uint32_t(g.ny());
    const float scale = *reinterpret_cast<const float*>(static_cast<uint8_t*>(terrain) + 0x34);
    if (!(scale >= 0.0f)) return false;
    Served& s = S(); std::lock_guard<std::mutex> l(s.m);
    Served::Grid* slot = s.Slot(g.base, n, false);
    if (!slot || slot->finished || slot->applied != n) return false;
    for (uint32_t i = 0; i < n; ++i)
        if (!TerrainSidecar::Eligible(TerrainSidecar::VectorOf(g.record(i))) || slot->range[i] == Served::kNone) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* r = g.record(i);
        *reinterpret_cast<float*>(r + 0x18) = float(slot->range[i] & 0xFFFF) * scale;
        *reinterpret_cast<float*>(r + 0x1c) = float(slot->range[i] >> 16) * scale;
        *reinterpret_cast<int32_t*>(r + 0x20) += 1;
    }
    slot->finished = true;
    return true;
}
// A served version whose pass has not come yet: the load builds two and passes
// each, so the first skip must not release the file (Windows 2026-09-27: the
// second version's last 4,859 AddTiles went unserved and its pass ran).
inline bool VersionPending() {
    Served& s = S(); std::lock_guard<std::mutex> l(s.m);
    for (const auto& g : s.grids) if (g.base && !g.finished) return true;
    return false;
}

// ---- AddTile: begin the armed sidecar, apply this tile ------------------------
using AddTileFn = void (*)(void*, int, uint64_t, uint64_t, uint64_t, uint64_t);
inline AddTileFn& OriginalAddTile() { static AddTileFn f = nullptr; return f; }
inline void*& LastTerrain() { static void* t = nullptr; return t; }
inline long FindRecord(const TerrainSidecar::Grid& g, int entity) {
    static std::atomic<uint32_t> cursor{0};
    const uint32_t n = g.nx() > 0 && g.ny() > 0 ? uint32_t(g.nx()) * uint32_t(g.ny()) : 0;
    if (!n) return -1;
    const uint32_t start = cursor.load() % n;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t i = start + k; if (i >= n) i -= n;
        const uint8_t* r = g.record(i);
        if (*reinterpret_cast<const int32_t*>(r) == entity && *reinterpret_cast<uint8_t* const*>(r + 8)) {
            cursor.store(i + 1 < n ? i + 1 : 0);
            return long(i);
        }
    }
    return -1;
}
inline void AddTileHook(void* terrain, int entity, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    OriginalAddTile()(terrain, entity, a, b, c, d);
    if (!terrain) return;
    LastTerrain() = terrain;
    static std::mutex beginLock;
    if (TerrainSidecar::g_pending) {
        std::lock_guard<std::mutex> l(beginLock);
        if (TerrainSidecar::g_pending) { S().Reset(); TerrainSidecar::BeginIfPending(terrain, nullptr); }
    }
    if (!TerrainSidecar::Loaded()) return;
    const TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
    if (!g.base || !g.records()) return;
    const long idx = FindRecord(g, entity);
    if (idx < 0 || !TerrainSidecar::Has(uint32_t(idx))) return;
    static thread_local BlockCodec::DecodeScratch* scratch = nullptr;
    if (!scratch) scratch = new (std::nothrow) BlockCodec::DecodeScratch;
    if (!scratch || !TerrainSidecar::ApplyTile(g, uint32_t(idx), *scratch)) return;
    Note(g, uint32_t(idx), TerrainSidecar::VectorOf(g.record(uint32_t(idx))));
}

// ---- SaveGame: capture at entry, stamp and rename after ----------------------
struct Ret16 { uint64_t a, b; };
using SaveFn = Ret16 (*)(void*, void*, void*, void*, void*, void*, const SaveGameId*, uint64_t, void*);
inline SaveFn& OriginalSave() { static SaveFn f = nullptr; return f; }
inline LogFn& Log() { static LogFn f = nullptr; return f; }
inline bool& WriteOn() { static bool on = true; return on; }

// Delete every <x>.terr / <x>.terr.tmp in `dir` whose <x>.sav is gone. Returns the count.
inline int SweepOrphans(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    int removed = 0;
    while (struct dirent* e = readdir(d)) {
        std::string f = e->d_name;
        std::string stem;
        if (f.size() > 9 && f.compare(f.size() - 9, 9, ".terr.tmp") == 0) stem = f.substr(0, f.size() - 9);
        else if (f.size() > 5 && f.compare(f.size() - 5, 5, ".terr") == 0) stem = f.substr(0, f.size() - 5);
        else continue;
        struct stat st{};
        if (stat((dir + "/" + stem + ".sav").c_str(), &st) != 0 && !remove((dir + "/" + f).c_str())) ++removed;
    }
    closedir(d);
    return removed;
}
// The .sav in `dir` written since `since` (an autosave's own name), newest first.
inline std::string WrittenSince(const std::string& dir, const struct timespec& since) {
    DIR* d = opendir(dir.c_str());
    if (!d) return std::string();
    std::string best; struct timespec bestT{};
    while (struct dirent* e = readdir(d)) {
        const size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".sav")) continue;
        struct timespec t{};
        const std::string p = dir + "/" + e->d_name;
        if (!Mtime(p, &t)) continue;
        const bool after = t.tv_sec > since.tv_sec || (t.tv_sec == since.tv_sec && t.tv_nsec >= since.tv_nsec);
        const bool newer = t.tv_sec > bestT.tv_sec || (t.tv_sec == bestT.tv_sec && t.tv_nsec > bestT.tv_nsec);
        if (after && (best.empty() || newer)) { best = p; bestT = t; }
    }
    closedir(d);
    return best;
}
inline Ret16 SaveHook(void* meta, void* shot, void* cfg, void* res, void* state, void* gui, const SaveGameId* id, uint64_t flag, void* monitor) {
    struct timespec callStart{}; clock_gettime(CLOCK_REALTIME, &callStart);
    std::string sav = WriteOn() ? SavPath(id) : std::string(), tmp;
    long tiles = -1; uint64_t bytes = 0; long long ms = 0;
    struct timespec before{}; const bool hadBefore = !sav.empty() && Mtime(sav, &before);
    void* terrain = LastTerrain();
    if (!sav.empty() && terrain) {
        const TerrainSidecar::Grid g = TerrainSidecar::GridOf(terrain);
        if (g.base && g.records() && g.nx() > 0 && g.ny() > 0) {
            char terr[1040]; TerrainSidecar::SidecarPath(sav.c_str(), terr, sizeof terr);
            if (terr[0]) {
                tmp = std::string(terr) + ".tmp";
                auto* enc = new (std::nothrow) BlockCodec::EncodeScratch;
                const auto t0 = std::chrono::steady_clock::now();
                if (enc) tiles = TerrainSidecar::Write(g, 0, tmp.c_str(), enc, &bytes);
                ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                delete enc;
            }
        }
    }
    const Ret16 r = OriginalSave()(meta, shot, cfg, res, state, gui, id, flag, monitor);
    if (tiles <= 0) { if (!tmp.empty()) remove(tmp.c_str()); return r; }
    struct timespec after{};
    bool fresh = Mtime(sav, &after) && (!hadBefore || after.tv_sec != before.tv_sec || after.tv_nsec != before.tv_nsec);
    if (!fresh) {   // an autosave the engine wrote under another name
        const std::string dir = sav.substr(0, sav.rfind('/'));
        const std::string written = WrittenSince(dir, callStart);
        if (!written.empty()) { sav = written; fresh = true; }
    }
    char terr[1040]; TerrainSidecar::SidecarPath(sav.c_str(), terr, sizeof terr);
    const uint64_t fp = fresh && terr[0] ? TerrainSidecar::HashFile(sav.c_str()) : 0;
    const bool ok = fp && TerrainSidecar::Refingerprint(tmp.c_str(), fp) && !rename(tmp.c_str(), terr);
    if (!ok) {
        remove(tmp.c_str());
        if (Log()) Log()("terrain sidecar: the save was not written or could not be hashed; the captured sidecar is discarded");
        return r;
    }
    const int swept = SweepOrphans(sav.substr(0, sav.rfind('/')));
    if (Log()) Log()("terrain sidecar: wrote %ld tiles, %.1f MiB, capture %lld ms on %u threads, beside %s%s", tiles, double(bytes) / 1048576.0,
                     ms, TerrainSidecar::WriteThreads(TerrainSidecar::g_writeThreads), sav.c_str(), swept ? " (sidecars of deleted saves removed)" : "");
    return r;
}

// ---- LoadGame: arm the sidecar for this save ----------------------------------
using LoadFn = void* (*)(void*, void*, void*, const SaveGameId*, void*, void*, void*, void*, void*, void*, void*);
inline LoadFn& OriginalLoad() { static LoadFn f = nullptr; return f; }
inline void* LoadHook(void* ret, void* ctx, void* mods, const SaveGameId* id, void* a4, void* a5, void* s0, void* s1, void* s2, void* s3, void* s4) {
    LastTerrain() = nullptr;
    S().Reset();
    TerrainSidecar::EndApply();
    TerrainSidecar::g_pending = false;
    const std::string sav = SavPath(id);
    if (!sav.empty()) {
        const auto t0 = std::chrono::steady_clock::now();
        TerrainSidecar::ArmForLoad(sav.c_str());
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (Log()) Log()("terrain sidecar: loading %s, fingerprint %016llx in %lld ms; %s", sav.c_str(),
                         (unsigned long long)TerrainSidecar::g_saveFingerprint, ms,
                         TerrainSidecar::g_pending && TerrainSidecar::FingerprintOfPath(TerrainSidecar::g_sidecarPath) == TerrainSidecar::g_saveFingerprint
                             ? TerrainSidecar::g_sidecarPath : "no matching sidecar");
        if (TerrainSidecar::g_pending && TerrainSidecar::FingerprintOfPath(TerrainSidecar::g_sidecarPath) != TerrainSidecar::g_saveFingerprint)
            TerrainSidecar::g_pending = false;
    } else if (Log()) Log()("terrain sidecar: this load's save path could not be resolved; loading stock");
    return OriginalLoad()(ret, ctx, mods, id, a4, a5, s0, s1, s2, s3, s4);
}

// ---- the alignment pass: skip it when the sidecar served every tile ----------
// Called from the redirected UpdateSubterrains call. True when the pass must
// not run (the caller then returns without calling it).
inline bool SkipPass(void* self, size_t blocks) {
    if (!self || blocks <= 512) return false;
    void* terrain = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 8);
    if (!AllServedFinish(terrain)) return false;
    if (Log()) Log()("alignment pass: %zu blocks skipped -- every tile was served from the sidecar (its min/max and version written from the served cache)", blocks);
    return true;
}
// After a load-sized pass: release the sidecar, unless it was skipped and
// another served version is still to pass.
inline void PassDone(size_t blocks, bool skipped) {
    if (blocks <= 512) return;
    if (skipped && VersionPending()) { if (Log()) Log()("terrain sidecar: pass skipped; kept for the load's other terrain version"); return; }
    const uint32_t applied = [] { Served& s = S(); std::lock_guard<std::mutex> l(s.m); uint32_t a = 0; for (auto& g : s.grids) a += g.applied; return a; }();
    if (TerrainSidecar::EndApply() && Log()) Log()("terrain sidecar: load done, %u tiles applied from the sidecar; file released", applied);
}
}  // namespace linux_sidecar
