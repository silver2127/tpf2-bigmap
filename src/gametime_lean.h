// gametime_lean.h -- GOG 2024-12-12: api.engine.getComponent(world, GAME_TIME)
// no longer copies the component's internal history vector into Lua.
//
// MEASURED 2026-11-22 on the user's 57 km map (tools: alloc_trace=1, the game
// console): one getComponent(world, GAME_TIME) costs ~0.75 ms, because the Lua
// binding (component switch 0xfa8740, case 16 = GAME_TIME -> 0xd65800)
// copy-constructs the component, including the std::vector of 16-byte entries
// at +0x18 (0xde270 -> 0xe6b40 -> operator new), several MiB on a long game.
// Lua only counts the userdata header, so its collector lets the copies pile
// up: the allocation sampler attributed 11 GiB live (35 GiB at a peak) to this
// one stack, and the process hit the commit limit ("Out of memory") while
// Timetables, Statistics++, CommonAPI2 and others read the clock every tick.
//
// The vector has no Lua property: the GameTime usertype exposes gameTime,
// gameTime0, updateCount and tickCount only (strings at 0x33f6078..0x33f6098),
// so a script can never see it. The fix retargets the single call at 0xd65885
// from the vector copy (0xde270) to the exe's own empty-vector constructor
// (0x98560: xor eax,eax / three zero stores / mov rax,rcx / ret). Every other
// field is copied as before; the copy's destructor frees an empty vector. The
// engine's own component is untouched. GOG only: the Steam site is unmeasured.
#pragma once
static int g_gameTimeLean = 1;   // gametime_lua_copy_lean

static bool InstallGameTimeLean() {
    if (!g_gameTimeLean) return false;
    if (!g_gog) { H->log("gametime lean copy: GOG 2024-12-12 only (Steam site not measured); OFF"); return false; }
    // lea rdx,[rsi+0x18] / call 0xde270 (vector copy) / mov ecx,[rsi+0x30]
    static const uint8_t kSite[] = {0x48, 0x8d, 0x56, 0x18, 0xe8, 0xe6, 0x89, 0x37, 0xff, 0x8b, 0x4e, 0x30};
    // xor eax,eax / mov [rcx],rax / mov [rcx+8],rax / mov [rcx+0x10],rax / mov rax,rcx / ret
    static const uint8_t kCtor[] = {0x33, 0xc0, 0x48, 0x89, 0x01, 0x48, 0x89, 0x41, 0x08, 0x48, 0x89, 0x41, 0x10, 0x48, 0x8b, 0xc1, 0xc3};
    // the case-16 entry of the component switch, so the site is the GAME_TIME copy
    static const uint8_t kCase[] = {0x48, 0x8b, 0xd7, 0x48, 0x8b, 0xcb, 0xe8, 0xe3, 0xc9, 0xdb, 0xff};   // fa8e12: mov rdx,rdi / mov rcx,rbx / call 0xd65800
    if (!H->verifyBytes(0xd65881, kSite, sizeof kSite) || !H->verifyBytes(0x98560, kCtor, sizeof kCtor) ||
        !H->verifyBytes(0xfa8e12, kCase, sizeof kCase)) {
        H->log("gametime lean copy: byte mismatch; OFF"); return false;
    }
    const int32_t rel = int32_t(int64_t(0x98560) - int64_t(0xd65885 + 5));
    uint8_t call[5] = {0xe8};
    memcpy(call + 1, &rel, 4);
    if (!H->patchBytes(0xd65885, call, sizeof call)) { H->log("gametime lean copy: patch failed; OFF"); return false; }
    H->log("gametime lean copy: getComponent(GAME_TIME) gives Lua the clock fields without the internal history vector (call 0xd65885 -> 0x98560)");
    return true;
}
extern "C" __declspec(dllexport) int BigmapTestInstallGameTimeLean(const Tpf2mpHost* host, int gog, int enabled) {
    const auto oldHost = H; const bool oldGog = g_gog; const int oldOn = g_gameTimeLean;
    H = host; g_gog = gog != 0; g_gameTimeLean = enabled;
    const bool ok = InstallGameTimeLean();
    H = oldHost; g_gog = oldGog; g_gameTimeLean = oldOn;
    return ok;
}
