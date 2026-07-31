#pragma once

#include <coreinit/mcp.h>
#include <coreinit/debug.h>
#include <whb/log.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstring>

#define LOG(fmt, ...) do { \
    OSReport("[UNINSTALLER] " fmt "\n", ##__VA_ARGS__); \
    WHBLogPrintf("[UNINSTALLER] " fmt, ##__VA_ARGS__); \
} while(0)

namespace TitleId {
    constexpr uint64_t GAME    = 0x00050000;
    constexpr uint64_t GAME_VC = 0x00050002;
    constexpr uint64_t DLC     = 0x0005000C;
    constexpr uint64_t UPDATE  = 0x0005000E;

    constexpr uint64_t High(uint64_t titleId) { return titleId >> 32; }
    constexpr uint64_t Low(uint64_t titleId)  { return titleId & 0xFFFFFFFF; }

    inline bool IsBaseGame(uint64_t titleId) {
        uint64_t high = High(titleId);
        return high == GAME || high == GAME_VC;
    }
    inline bool IsVirtualConsole(uint64_t titleId) { return High(titleId) == GAME_VC; }
    inline bool IsUpdate(uint64_t titleId) { return High(titleId) == UPDATE; }
    inline bool IsDLC(uint64_t titleId)    { return High(titleId) == DLC; }
}

namespace CachePath {
    inline const char* USB()   { return "fs:/vol/external01/wiiu/apps/WiiUTitleUninstaller/cache_usb.txt"; }
    inline const char* NAND()  { return "fs:/vol/external01/wiiu/apps/WiiUTitleUninstaller/cache_nand.txt"; }
    inline const char* Prefs() { return "fs:/vol/external01/wiiu/apps/WiiUTitleUninstaller/prefs.txt"; }
}

inline bool TitleOnUSB(const MCPTitleListType& info) {
    return strstr(info.indexedDevice, "usb") != nullptr;
}
inline bool TitleOnNAND(const MCPTitleListType& info) {
    return strstr(info.indexedDevice, "mlc") != nullptr;
}

inline void EnsureCacheDir() {
    mkdir("fs:/vol/external01/wiiu",                      0777);
    mkdir("fs:/vol/external01/wiiu/apps",                 0777);
    mkdir("fs:/vol/external01/wiiu/apps/WiiUTitleUninstaller", 0777);
}
