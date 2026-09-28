// sidecar_linux.h end to end on a fake terrain: a save writes "<name>.terr"
// beside "<name>.sav", a load of that save serves every tile at AddTile and the
// pass is skipped with each record's min/max/version written as publication
// would; a changed save, a partial load and a missing tile all refuse the skip.
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../sidecar_linux.h"

namespace {
constexpr int NX = 3, NY = 2, N = NX * NY;
struct Fake {
    uint8_t terrain[0x40]{};
    uint8_t grid[0x18]{};
    uint8_t records[N * 40]{};
    TerrainSidecar::TileVector vec[N]{};
    std::vector<uint16_t> heights[N];
    Fake() {
        *reinterpret_cast<uint8_t**>(terrain + 0x18) = grid;
        *reinterpret_cast<float*>(terrain + 0x34) = 0.5f;
        int32_t g[4] = {10, 20, NX, NY};
        memcpy(grid, g, 16);
        *reinterpret_cast<uint8_t**>(grid + 0x10) = records;
        for (int i = 0; i < N; ++i) {
            heights[i].resize(TerrainSidecar::Samples);
            vec[i] = {heights[i].data(), heights[i].data() + heights[i].size(), heights[i].data() + heights[i].size()};
            *reinterpret_cast<int32_t*>(records + i * 40) = 1000 + i;   // entity
            *reinterpret_cast<TerrainSidecar::TileVector**>(records + i * 40 + 8) = &vec[i];
        }
    }
    void Fill(unsigned seed) {
        for (int i = 0; i < N; ++i)
            for (size_t k = 0; k < heights[i].size(); ++k) heights[i][k] = uint16_t(100 * i + (k * 7 + seed) % 97);
    }
    void Clear() { for (auto& h : heights) std::fill(h.begin(), h.end(), 0); }
    float MinZ(int i) const { return *reinterpret_cast<const float*>(records + i * 40 + 0x18); }
    float MaxZ(int i) const { return *reinterpret_cast<const float*>(records + i * 40 + 0x1c); }
    int32_t Version(int i) const { return *reinterpret_cast<const int32_t*>(records + i * 40 + 0x20); }
};

std::string g_dir;
std::string g_savContent = "save v1";
int g_logs = 0;
void Log(const char* fmt, ...) { ++g_logs; va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); puts(""); }
void FakeAddTile(void*, int, uint64_t, uint64_t, uint64_t, uint64_t) {}
linux_sidecar::Ret16 FakeSave(void*, void*, void*, void*, void*, void*, const linux_sidecar::SaveGameId* id, uint64_t, void*) {
    FILE* f = fopen((g_dir + "/" + std::string(id->name.p, id->name.n) + ".sav").c_str(), "wb");
    fwrite(g_savContent.data(), 1, g_savContent.size(), f); fclose(f);
    return {0, 0};
}
void* FakeLoad(void* ret, void*, void*, const linux_sidecar::SaveGameId*, void*, void*, void*, void*, void*, void*, void*) { return ret; }

linux_sidecar::SaveGameId Id(const char* name) {
    linux_sidecar::SaveGameId id{};
    id.path.p = id.path.buf; id.ns.p = id.ns.buf;
    id.name.p = name; id.name.n = strlen(name);
    return id;
}
void Load(Fake& f, const linux_sidecar::SaveGameId& id, int tiles) {
    linux_sidecar::LoadHook(nullptr, nullptr, nullptr, &id, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    for (int i = 0; i < tiles; ++i) linux_sidecar::AddTileHook(f.terrain, 1000 + i, 0, 0, 0, 0);
}
bool Pass(Fake& f, size_t blocks = 4096) {
    void* self[2] = {nullptr, f.terrain};
    const bool skipped = linux_sidecar::SkipPass(self, blocks);
    linux_sidecar::PassDone(blocks, skipped);
    return skipped;
}
}  // namespace

int main() {
    char tmpl[] = "/tmp/sidecar_test_XXXXXX";
    assert(mkdtemp(tmpl));
    const std::string local = tmpl;
    g_dir = local + "/save";
    assert(!mkdir(g_dir.c_str(), 0755));
    setenv("TPF2MP_USERDATA", local.c_str(), 1);
    linux_sidecar::Log() = Log;
    linux_sidecar::OriginalAddTile() = FakeAddTile;
    linux_sidecar::OriginalSave() = FakeSave;
    linux_sidecar::OriginalLoad() = FakeLoad;

    const auto id = Id("ServerSave");
    assert(linux_sidecar::SavPath(&id) == g_dir + "/ServerSave.sav");
    auto bad = Id("../x"); assert(linux_sidecar::SavPath(&bad).empty());

    Fake f;
    f.Fill(3);
    std::vector<uint16_t> want[N];
    for (int i = 0; i < N; ++i) want[i] = f.heights[i];

    // 1. The save: a tile must exist in AddTile's terrain first.
    linux_sidecar::AddTileHook(f.terrain, 1000, 0, 0, 0, 0);
    linux_sidecar::SaveHook(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &id, 0, nullptr);
    const std::string terr = g_dir + "/ServerSave.terr";
    struct stat st{};
    assert(!stat(terr.c_str(), &st) && st.st_size > 0);
    assert(stat((terr + ".tmp").c_str(), &st) != 0);
    assert(TerrainSidecar::FingerprintOfPath(terr.c_str()) == TerrainSidecar::HashFile((g_dir + "/ServerSave.sav").c_str()));

    // 2. A full load: every tile restored, the pass skipped, min/max/version as publication.
    f.Clear();
    Load(f, id, N);
    for (int i = 0; i < N; ++i) assert(f.heights[i] == want[i]);
    assert(Pass(f));
    for (int i = 0; i < N; ++i) {
        const auto mm = std::minmax_element(want[i].begin(), want[i].end());
        assert(f.MinZ(i) == float(*mm.first) * 0.5f && f.MaxZ(i) == float(*mm.second) * 0.5f && f.Version(i) == 1);
    }
    assert(!TerrainSidecar::Loaded());

    // 3. A partial load (one tile never added) runs the pass and changes no record.
    f.Clear();
    Load(f, id, N - 1);
    assert(f.heights[0] == want[0]);
    assert(!Pass(f));
    for (int i = 0; i < N; ++i) assert(f.Version(i) == 1);

    // 4. A tile gone from the terrain (no vector) refuses too.
    f.Clear();
    Load(f, id, N);
    *reinterpret_cast<void**>(f.records + 5 * 40 + 8) = nullptr;
    assert(!Pass(f));
    *reinterpret_cast<void**>(f.records + 5 * 40 + 8) = &f.vec[5];

    // 5. A small pass (an in-game edit) never skips and leaves the sidecar alone.
    f.Clear();
    Load(f, id, N);
    assert(!Pass(f, 100) && TerrainSidecar::Loaded());
    assert(Pass(f));

    // 6. The save changed on disk: the fingerprint no longer matches, nothing served.
    g_savContent = "save v2";
    { FILE* s = fopen((g_dir + "/ServerSave.sav").c_str(), "wb"); fwrite("save v2", 1, 7, s); fclose(s); }
    f.Clear();
    Load(f, id, N);
    assert(f.heights[0][1] == 0 && !Pass(f));

    // 7. An orphaned sidecar is swept by the next save.
    { FILE* o = fopen((g_dir + "/gone.terr").c_str(), "wb"); fputs("x", o); fclose(o); }
    f.Fill(5);
    linux_sidecar::SaveHook(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &id, 0, nullptr);
    assert(stat((g_dir + "/gone.terr").c_str(), &st) != 0);
    assert(TerrainSidecar::FingerprintOfPath(terr.c_str()) == TerrainSidecar::HashFile((g_dir + "/ServerSave.sav").c_str()));

    // 9. Two terrain versions, as a real load builds: the first skip keeps the
    //    file for the second, both skip, and neither skips twice.
    {
        g_savContent = "save v3";
        f.Fill(7);
        linux_sidecar::WriteOn() = true;
        linux_sidecar::SaveHook(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &id, 0, nullptr);
        Fake second;
        f.Clear();
        Load(f, id, N);
        for (int i = 0; i < N; ++i) linux_sidecar::AddTileHook(second.terrain, 1000 + i, 0, 0, 0, 0);
        const int32_t v0 = f.Version(0);
        assert(Pass(f) && TerrainSidecar::Loaded() && f.Version(0) == v0 + 1);
        assert(!Pass(f) && TerrainSidecar::Loaded() == false);   // the same version again: runs (and a pass that runs releases)
        f.Clear(); second.Clear();
        Load(f, id, N);
        for (int i = 0; i < N; ++i) linux_sidecar::AddTileHook(second.terrain, 1000 + i, 0, 0, 0, 0);
        assert(Pass(f) && TerrainSidecar::Loaded());
        assert(Pass(second) && !TerrainSidecar::Loaded());
    }

    // 8. Writing off: a save leaves no sidecar.
    linux_sidecar::WriteOn() = false;
    const auto other = Id("Other");
    linux_sidecar::SaveHook(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &other, 0, nullptr);
    assert(stat((g_dir + "/Other.terr").c_str(), &st) != 0);

    std::string rm = "rm -rf " + local; assert(!system(rm.c_str()));
    puts("PASS Linux terrain sidecar: save writes it, a full load serves every tile and skips the pass; partial, missing-tile, small-pass and changed-save cases fall back");
}
