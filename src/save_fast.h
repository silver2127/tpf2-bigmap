// Steam 35924 Serializer.cpp PushCompressor, used only by save stream setup.
// Independent patches: compression level 3 -> 1 and input buffer 128 -> 65536.
// No changes to serializer fields, error handling, finalization or file rotation.
//
// GOG 2024-12-12: the same function at +0x220 (installed through
// RunWithSteamSites). The level load is RIP-relative, so its displacement is
// the build's own: GOG reads the same global through 0x362b5f5 (measured,
// tools/test_gog_sites.py). The patch overwrites the whole instruction.
#pragma once
static bool g_saveFast = false;
struct SavePatch { uintptr_t rva; uint8_t before[6], gogBefore[6], after[6]; };
static const SavePatch kSavePatches[] = {
    {0x2e8641,{0x8b,0x05,0x35,0xf0,0x63,0x03},{0x8b,0x05,0xf5,0xb5,0x62,0x03},{0xb8,0x01,0,0,0,0x90}},
    {0x2e87ad,{0x41,0xb8,0x80,0,0,0},{0x41,0xb8,0x80,0,0,0},{0x41,0xb8,0,0,0x01,0}}
};
static bool InstallSaveFast() {
    if (!g_saveFast) return false;
    for (const auto& p : kSavePatches) {
        if (!H->verifyBytes(p.rva,g_gog?p.gogBefore:p.before,6)) {
            H->log("fast saves: byte mismatch; OFF"); return false;
        }
    }
    int done=0;
    for (const auto& p : kSavePatches) {
        if (!H->patchBytes(p.rva,p.after,6)) {
            // Each change is independently valid, but try to restore the
            // previous one rather than silently claiming complete activation.
            bool restored=true;
            while (done) {
                const auto& old=kSavePatches[--done];
                if (!H->patchBytes(old.rva,g_gog?old.gogBefore:old.before,6)) restored=false;
            }
            H->log("fast saves: patch failed; rollback %s",restored?"OK":"FAILED (partial optimization)");
            return false;
        }
        ++done;
    }
    H->log("fast saves: zstd level 1, 64 KiB input buffer; save format unchanged, files may be larger");
    return true;
}
extern "C" __declspec(dllexport)
int BigmapTestInstallSaveFast(const Tpf2mpHost* host,int gog,int enabled) {
    const auto oldHost=H;const bool oldGog=g_gog,oldEnabled=g_saveFast;
    H=host;g_gog=gog!=0;g_saveFast=enabled!=0;
    bool ok=RunWithSteamSites([]{return InstallSaveFast();});
    H=oldHost;g_gog=oldGog;g_saveFast=oldEnabled;
    return ok;
}
