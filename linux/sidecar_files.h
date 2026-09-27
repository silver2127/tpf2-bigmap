// Native file discovery for the autosave sidecar protocol. No engine hooks.
#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <string>
#include <ctime>

namespace linux_sidecar {
// Find exactly one regular .sav modified since callStart minus the upstream
// two-second filesystem timestamp allowance. UTF-8 is passed to POSIX unchanged.
// The caller must serialize saves and confirm save success before stamping a
// sidecar; this timestamp heuristic is not proof of terrain ownership.
inline bool WrittenDuringCall(const char* resolved, const timespec& callStart,
                              char* out, size_t cap) {
    if (!resolved || !out || !cap) return false;
    const char* slash = std::strrchr(resolved, '/');
    if (!slash) return false;
    const std::string dir(resolved, size_t(slash - resolved + 1));
    DIR* entries = opendir(dir.c_str());
    if (!entries) return false;
    timespec floor = callStart;
    floor.tv_sec -= 2;
    std::string candidate;
    unsigned found = 0;
    while (dirent* entry = readdir(entries)) {
        const size_t n = std::strlen(entry->d_name);
        if (n < 5 || std::strcmp(entry->d_name + n - 4, ".sav")) continue;
        const std::string path = dir + entry->d_name;
        struct stat info{};
        // Reject symlinks as well as directories; do not adopt a foreign target.
        if (lstat(path.c_str(), &info) || !S_ISREG(info.st_mode)) continue;
        if (info.st_mtim.tv_sec < floor.tv_sec ||
            (info.st_mtim.tv_sec == floor.tv_sec && info.st_mtim.tv_nsec < floor.tv_nsec)) continue;
        candidate = path;
        if (++found > 1) break;
    }
    closedir(entries);
    if (found != 1 || candidate.size() >= cap) return false;
    std::memcpy(out, candidate.c_str(), candidate.size() + 1);
    return true;
}
} // namespace linux_sidecar
