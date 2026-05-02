#include "TitleUninstaller.hpp"
#include "Gfx.hpp"
#include "Input.hpp"
#include <coreinit/debug.h>
#include <coreinit/filesystem.h>
#include <coreinit/mcp.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/proc.h>
#include <nn/acp.h>
#include <dirent.h>
#include <sys/stat.h>
#include <malloc.h>
#include <algorithm>
#include <cstring>
#include <cinttypes>
#include <cmath>

#define LOG(fmt, ...) do { \
    OSReport("[UNINSTALLER] " fmt "\n", ##__VA_ARGS__); \
    WHBLogPrintf("[UNINSTALLER] " fmt, ##__VA_ARGS__); \
} while(0)

TitleEntry::~TitleEntry() {
    if (icon) {
        OSReport("[EXIT] SDL_DestroyTexture for title '%s'\n", name.c_str());
        SDL_DestroyTexture(icon);
        icon = nullptr;
    }
}

TitleUninstaller::TitleUninstaller()
    : state(AppState::Loading),
      selectedIndex(0), loadingScreenShown(false),
      sortMode(SortMode::Alphabetical),
      currentStorage(StorageLocation::USB),
      themeMode(ThemeMode::Dark), settingsSelectedItem(0),
      uninstallCurrent(0), uninstallSucceeded(0), uninstallFailed(0),
      uninstallInProgress(false), uninstallSeenActive(false), uninstallPollFrames(0), mcpHandle(-1),
      usbTotalBytes(0), usbFreeBytes(0),
      lastTick(0), selectionPulse(0.0f), scrollAnim(0.0f), targetScroll(0),
      highlightBarAnim(0.0f), holdTimer(0.0f), repeatAccum(0.0f)
{
    memset(&mcpTitleInfo, 0, sizeof(mcpTitleInfo));

    // Applies the dark theme by default, then loads saved prefs
    Gfx::SetTheme(Gfx::MakeDarkTheme());
    LoadPrefs();

    // Attempt cache load immediately when available — if it succeeds we go straight to the game List
    // with no loading screen flash when initialising.
    if (LoadTitleCache()) {
        LOG("Cache hit: %zu titles, skipping scan", titles.size());
        QueryUSBStorage();
        state = AppState::List;
    }
}

TitleUninstaller::~TitleUninstaller() {
    OSReport("[EXIT] ~TitleUninstaller begin\n");
    OSReport("[EXIT] Closing MCP handle=%d\n", mcpHandle);
    if (mcpHandle >= 0) { MCP_Close(mcpHandle); mcpHandle = -1; }
    OSReport("[EXIT] MCP_Close done\n");
    OSReport("[EXIT] Clearing %zu title entries\n", titles.size());
    titles.clear();
    titlesUSB.clear();
    titlesNAND.clear();
    OSReport("[EXIT] ~TitleUninstaller complete\n");
}

void TitleUninstaller::StopIconThread() {
    OSReport("[EXIT] StopIconThread called (no-op, no thread)\n");
}

std::string TitleUninstaller::FormatSize(uint64_t bytes) const {
    char buf[32];
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)(bytes / 1024));
    }
    return buf;
}

int TitleUninstaller::CheckedCount() const {
    int n = 0;
    for (const auto& t : titles) if (t->checked) n++;
    return n;
}

uint64_t TitleUninstaller::CheckedBytes() const {
    uint64_t total = 0;
    for (const auto& t : titles) if (t->checked) total += t->sizeBytes;
    return total;
}

void TitleUninstaller::ApplySort() {
    // Remembers which title was selected so it will follow it after sort
    TitleEntry* selected = (selectedIndex >= 0 && selectedIndex < (int)titles.size())
                           ? titles[selectedIndex].get() : nullptr;

    switch (sortMode) {
    case SortMode::Alphabetical:
        std::stable_sort(titles.begin(), titles.end(),
            [](const std::unique_ptr<TitleEntry>& a, const std::unique_ptr<TitleEntry>& b) {
                return a->name < b->name;
            });
        break;
    case SortMode::SizeDesc:
        std::stable_sort(titles.begin(), titles.end(),
            [](const std::unique_ptr<TitleEntry>& a, const std::unique_ptr<TitleEntry>& b) {
                return a->sizeBytes > b->sizeBytes;
            });
        break;
    case SortMode::SizeAsc:
        std::stable_sort(titles.begin(), titles.end(),
            [](const std::unique_ptr<TitleEntry>& a, const std::unique_ptr<TitleEntry>& b) {
                return a->sizeBytes < b->sizeBytes;
            });
        break;
    default: break;
    }

    if (selected) {
        for (int i = 0; i < (int)titles.size(); i++) {
            if (titles[i].get() == selected) {
                selectedIndex = i;
                // Clamp scroll
                if (selectedIndex < targetScroll)
                    targetScroll = selectedIndex;
                else if (selectedIndex >= targetScroll + VISIBLE_ROWS)
                    targetScroll = selectedIndex - VISIBLE_ROWS + 1;
                break;
            }
        }
    }
}

static std::string VolPathToFsPath(const std::string& volPath) {
    if (volPath.find("/vol/") == 0) {
        size_t slash = volPath.find('/', 5);
        if (slash != std::string::npos) {
            std::string device = volPath.substr(5, slash - 5);
            std::string rest   = volPath.substr(slash);
            return device + ":" + rest;
        }
    }
    return volPath;
}

void TitleUninstaller::LoadTitleMetadata(TitleEntry& t) {
    std::string base = VolPathToFsPath(t.path);

    // Get app name using ACPGetTitleMetaXml (0x40-aligned)
    ACPMetaXml* meta = static_cast<ACPMetaXml*>(memalign(0x40, sizeof(ACPMetaXml)));
    if (meta) {
        memset(meta, 0, sizeof(ACPMetaXml));
        if (ACPGetTitleMetaXml(t.titleId, meta) == 0) {
            if (meta->shortname_en[0] != '\0')
                t.name = meta->shortname_en;
        }
        free(meta);
    }

    if (t.sizeBytes == 0) {
        auto sumDir = [](const std::string& dirPath) -> uint64_t {
            uint64_t total = 0;
            DIR* d = opendir(dirPath.c_str());
            if (!d) return 0;
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                if (e->d_name[0] == '.') continue;
                std::string fp = dirPath + "/" + e->d_name;
                struct stat st;
                if (stat(fp.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                    total += (uint64_t)st.st_size;
            }
            closedir(d);
            return total;
        };

        t.sizeBytes  = sumDir(base + "/content");
        t.sizeBytes += sumDir(base + "/code");
        t.sizeBytes += sumDir(base + "/meta");
    }

    // Fall back to meta.xml for name if ACP didn't provide one
    if (t.name.empty()) {
        std::string xmlPath = base + "/meta/meta.xml";
        FILE* f = fopen(xmlPath.c_str(), "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                const char* tag = strstr(line, "shortname_en");
                if (tag) {
                    const char* s = strchr(tag, '>');
                    const char* e = s ? strchr(s + 1, '<') : nullptr;
                    if (s && e && e > s + 1) {
                        std::string candidate(s + 1, e - s - 1);
                        while (!candidate.empty() &&
                               (candidate.back() == '\n' || candidate.back() == '\r' || candidate.back() == ' '))
                            candidate.pop_back();
                        if (!candidate.empty()) { t.name = candidate; break; }
                    }
                }
            }
            fclose(f);
        }
    }

    // Hex title ID as a last resort if nothing else works: 
    if (t.name.empty()) {
        char buf[17];
        snprintf(buf, sizeof(buf), "%016" PRIx64, t.titleId);
        t.name = buf;
    }

    t.iconPath   = base + "/meta/iconTex.tga";
    t.iconLoaded = false;
}

static const char* CACHE_PATH_USB  = "fs:/vol/external01/wiiu/apps/WiiUTitleUninstaller/cache_usb.txt";
static const char* CACHE_PATH_NAND = "fs:/vol/external01/wiiu/apps/WiiUTitleUninstaller/cache_nand.txt";
static const char* PREFS_PATH      = "fs:/vol/external01/wiiu/apps/WiiUTitleUninstaller/prefs.txt";

static void EnsureCacheDir() {
    mkdir("fs:/vol/external01/wiiu",                                    0777);
    mkdir("fs:/vol/external01/wiiu/apps",                               0777);
    mkdir("fs:/vol/external01/wiiu/apps/WiiUTitleUninstaller",          0777);
}

void TitleUninstaller::SavePrefs() {
    EnsureCacheDir();
    FILE* f = fopen(PREFS_PATH, "w");
    if (!f) return;
    fprintf(f, "theme=%d\n", (int)themeMode);
    fclose(f);
}

void TitleUninstaller::LoadPrefs() {
    FILE* f = fopen(PREFS_PATH, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int val = 0;
        if (sscanf(line, "theme=%d", &val) == 1) {
            themeMode = (val == (int)ThemeMode::Light) ? ThemeMode::Light : ThemeMode::Dark;
            if (themeMode == ThemeMode::Light)
                Gfx::SetTheme(Gfx::MakeLightTheme());
            else
                Gfx::SetTheme(Gfx::MakeDarkTheme());
        }
    }
    fclose(f);
}

void TitleUninstaller::SaveTitleCache() {
    EnsureCacheDir();
    const char* path = (currentStorage == StorageLocation::USB) ? CACHE_PATH_USB : CACHE_PATH_NAND;
    FILE* f = fopen(path, "w");
    if (!f) { LOG("SaveTitleCache: fopen failed"); return; }

    // Count header so LoadTitleCache can detect changes fast
    fprintf(f, "#count=%zu\n", titles.size());

    for (const auto& t : titles) {
        std::string safeName = t->name;
        for (char& c : safeName) if (c == '|') c = ' ';
        fprintf(f, "%016" PRIx64 "|%s|%" PRIu64 "|%s\n",
                t->titleId, t->path.c_str(), t->sizeBytes, safeName.c_str());
    }
    fclose(f);
    LOG("SaveTitleCache: wrote %zu entries", titles.size());
}

// Returns the number of base-game titles on the given storage device.
static int GetGameCount(bool forUSB) {
    int32_t handle = MCP_Open();
    if (handle < 0) return -1;

    uint32_t total = MCP_TitleCount(handle);
    int count = 0;

    if (total > 0) {
        std::vector<MCPTitleListType> list(total);
        uint32_t fetched  = total;
        MCP_TitleList(handle, &fetched, list.data(), fetched * sizeof(MCPTitleListType));
        for (uint32_t i = 0; i < fetched; i++) {
            if (list[i].appType != MCP_APP_TYPE_GAME) continue;
            bool onUSB  = (strstr(list[i].indexedDevice, "usb") != nullptr);
            bool onNAND = (strstr(list[i].indexedDevice, "mlc") != nullptr);
            if (forUSB  && onUSB)  count++;
            if (!forUSB && onNAND) count++;
        }
    }

    MCP_Close(handle);
    return count;
}

bool TitleUninstaller::LoadTitleCache() {
    const char* path = (currentStorage == StorageLocation::USB) ? CACHE_PATH_USB : CACHE_PATH_NAND;
    FILE* f = fopen(path, "r");
    if (!f) return false;

    char header[64] = {};
    if (!fgets(header, sizeof(header), f)) { fclose(f); return false; }

    int cachedCount = 0;
    if (sscanf(header, "#count=%d", &cachedCount) != 1) {
        fclose(f);
        remove(path);
        return false;
    }

    // When the number of installed games has changed,
    // the cache is stale — force a full rescan.
    int currentCount = GetGameCount(currentStorage == StorageLocation::USB);
    if (currentCount < 0 || currentCount != cachedCount) {
        LOG("Cache stale: cached=%d current=%d — rescanning", cachedCount, currentCount);
        fclose(f);
        remove(path);
        return false;
    }

    std::vector<std::unique_ptr<TitleEntry>> cached;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        char* p1 = strchr(line, '|');           if (!p1) continue; *p1 = '\0';
        char* p2 = strchr(p1 + 1, '|');         if (!p2) continue; *p2 = '\0';
        char* p3 = strchr(p2 + 1, '|');         if (!p3) continue; *p3 = '\0';

        uint64_t titleId = 0;
        if (sscanf(line, "%" SCNx64, &titleId) != 1 || titleId == 0) continue;

        uint64_t sizeBytes = 0;
        sscanf(p2 + 1, "%" SCNu64, &sizeBytes);

        auto t = std::make_unique<TitleEntry>();
        t->titleId   = titleId;
        t->path      = p1 + 1;
        t->sizeBytes = sizeBytes;
        t->name      = p3 + 1;
        t->kind      = TitleKind::Game;
        cached.push_back(std::move(t));
    }
    fclose(f);

    if (cached.empty()) return false;

    bool anyRemoved = false;
    std::vector<std::unique_ptr<TitleEntry>> valid;
    valid.reserve(cached.size());

    for (auto& t : cached) {
        std::string fsPath = VolPathToFsPath(t->path);
        struct stat st;
        if (stat(fsPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            t->iconPath   = fsPath + "/meta/iconTex.tga";
            t->iconLoaded = false;
            valid.push_back(std::move(t));
        } else {
            LOG("Cache: removed stale entry '%s'", t->name.c_str());
            anyRemoved = true;
        }
    }

    if (valid.empty()) return false;

    titles = std::move(valid);

    if (anyRemoved) SaveTitleCache();

    return true;
}

void TitleUninstaller::QueryUSBStorage() {
    QueryStorage();
}

void TitleUninstaller::QueryStorage() {
    // Use FSGetFreeSpaceSize (documented on wiiubrew.org/wiki/Coreinit.rpl)
    // to get the free space on the active storage device.
    // FSGetStat on the root path gives quotaSize = total allocated space.
    const char* mountPath = (currentStorage == StorageLocation::USB)
                            ? "/vol/storage_usb01" : "/vol/storage_mlc01";

    FSClient*   client = static_cast<FSClient*>(memalign(0x40, sizeof(FSClient)));
    FSCmdBlock* block  = static_cast<FSCmdBlock*>(memalign(0x40, sizeof(FSCmdBlock)));

    if (!client || !block) {
        if (client) free(client);
        if (block)  free(block);
        usbTotalBytes = 0;
        usbFreeBytes  = 0;
        return;
    }

    FSInit();
    FSAddClient(client, FS_ERROR_FLAG_NONE);
    FSInitCmdBlock(block);

    // Free space
    uint64_t freeSize = 0;
    FSStatus freeResult = FSGetFreeSpaceSize(client, block, mountPath,
                                             &freeSize, FS_ERROR_FLAG_NONE);

    // Total space via quotaSize from FSGetStat on the mount root
    uint64_t totalSize = 0;
    FSStat stat;
    memset(&stat, 0, sizeof(stat));
    FSStatus statResult = FSGetStat(client, block, mountPath, &stat, FS_ERROR_FLAG_NONE);
    if (statResult == FS_STATUS_OK && stat.quotaSize > 0)
        totalSize = stat.quotaSize;

    FSDelClient(client, FS_ERROR_FLAG_NONE);
    FSShutdown();
    free(client);
    free(block);

    if (freeResult == FS_STATUS_OK) {
        usbFreeBytes  = freeSize;
        usbTotalBytes = (totalSize > 0) ? totalSize : freeSize; // fallback
        LOG("Storage (%s): total=%llu free=%llu", mountPath,
            (unsigned long long)usbTotalBytes,
            (unsigned long long)usbFreeBytes);
    } else {
        LOG("FSGetFreeSpaceSize failed for %s: %d", mountPath, (int)freeResult);
        usbTotalBytes = 0;
        usbFreeBytes  = 0;
    }
}

void TitleUninstaller::LoadTitles() {
    ACPInitialize();

    mcpHandle = MCP_Open();
    if (mcpHandle < 0) {
        LOG("MCP_Open failed");
        ACPFinalize();
        return;
    }

    uint32_t count = MCP_TitleCount(mcpHandle);
    LOG("MCP_TitleCount = %u", count);

    if (count > 0) {
        std::vector<MCPTitleListType> list(count);
        uint32_t fetched  = count;
        uint32_t byteSize = count * sizeof(MCPTitleListType);
        MCP_TitleList(mcpHandle, &fetched, list.data(), byteSize);

        for (uint32_t i = 0; i < fetched; i++) {
            const auto& info = list[i];

            bool isGame   = (info.appType == MCP_APP_TYPE_GAME);
            bool isUpdate = (info.appType == MCP_APP_TYPE_GAME_UPDATE);
            bool isDLC    = (info.appType == MCP_APP_TYPE_GAME_DLC);

            // Only show base games — updates and DLC are left untouched.
            // Save data is a separate title type and is never affected.
            (void)isUpdate; (void)isDLC;
            if (!isGame) continue;

            // Filter by active storage location
            bool onUSB  = (strstr(info.indexedDevice, "usb") != nullptr);
            bool onNAND = (strstr(info.indexedDevice, "mlc") != nullptr);
            if (currentStorage == StorageLocation::USB  && !onUSB)  continue;
            if (currentStorage == StorageLocation::NAND && !onNAND) continue;

            auto t = std::make_unique<TitleEntry>();
            t->titleId = info.titleId;
            t->path    = info.path;
            t->kind    = TitleKind::Game;

            char buf[17];
            snprintf(buf, sizeof(buf), "%016" PRIx64, t->titleId);
            t->name = buf;

            LoadTitleMetadata(*t);
            titles.push_back(std::move(t));

            if (titles.size() % 10 == 0) {
                WHBProcIsRunning();
            }
        }
    }

    // Apply current sort mode
    ApplySort();

    // Close the scan handle — Start Uninstall opens a fresh one
    MCP_Close(mcpHandle);
    mcpHandle = -1;

    ACPFinalize();

    // Query storage space via FSGetFreeSpaceSize (coreinit FS API)
    QueryUSBStorage();

    LOG("Loaded %zu USB titles", titles.size());

    // Save to cache for next launch
    SaveTitleCache();
}

void TitleUninstaller::LoadNextPendingIcon() {
    int first = targetScroll;
    int last  = std::min(first + VISIBLE_ROWS + 1, (int)titles.size());

    for (int i = first; i < last; i++) {
        if (!titles[i]->iconLoaded) {
            titles[i]->iconLoaded = true;
            const std::string& path = titles[i]->iconPath;
            if (!path.empty()) {
                SDL_Surface* surf = IMG_Load(path.c_str());
                if (surf) {
                    titles[i]->icon = SDL_CreateTextureFromSurface(Gfx::GetRenderer(), surf);
                    SDL_FreeSurface(surf);
                }
            }
            return;
        }
    }
}

// Uninstall logic

void TitleUninstaller::StartUninstall() {
    uninstallQueue.clear();
    for (int i = 0; i < (int)titles.size(); i++) {
        if (titles[i]->checked) uninstallQueue.push_back(i);
    }
    uninstallCurrent    = 0;
    uninstallSucceeded  = 0;
    uninstallFailed     = 0;
    uninstallInProgress = false;
    uninstallSeenActive = false;
    uninstallPollFrames = 0;

    // Open a fresh MCP handle for the uninstall operations
    if (mcpHandle >= 0) { MCP_Close(mcpHandle); mcpHandle = -1; }
    mcpHandle = MCP_Open();
    if (mcpHandle < 0) {
        LOG("StartUninstall: MCP_Open failed");
        return;
    }

    state = AppState::Uninstalling;
    LOG("Starting batch uninstall of %zu titles", uninstallQueue.size());
}

bool TitleUninstaller::UninstallNext() {
    if (uninstallCurrent >= (int)uninstallQueue.size()) {
        if (mcpHandle >= 0) { MCP_Close(mcpHandle); mcpHandle = -1; }

        // Remove successfully uninstalled titles from the in-memory list
        // and rewrite the cache — no full rescan needed
        std::vector<std::unique_ptr<TitleEntry>> remaining;
        for (auto& t : titles) {
            if (!t->checked) {
                remaining.push_back(std::move(t));
            }
        }
        titles = std::move(remaining);

        // Clamp selection
        if (selectedIndex >= (int)titles.size())
            selectedIndex = std::max(0, (int)titles.size() - 1);
        if (targetScroll >= (int)titles.size())
            targetScroll = std::max(0, (int)titles.size() - 1);

        SaveTitleCache();

        state = AppState::Done;
        return false;
    }

    int idx = uninstallQueue[uninstallCurrent];
    TitleEntry& t = *titles[idx];

    if (!uninstallInProgress) {
        // Safety check — never uninstall updates, DLC, or save data
        if (t.kind != TitleKind::Game) {
            LOG("Skipping non-game title: %s (kind=%d)", t.name.c_str(), (int)t.kind);
            uninstallFailed++;
            uninstallCurrent++;
            return true;
        }
        LOG("Uninstalling: %s  path=%s", t.name.c_str(), t.path.c_str());
        memset(&mcpTitleInfo, 0, sizeof(mcpTitleInfo));
        MCPError err = MCP_UninstallTitleAsync(mcpHandle, t.path.c_str(), &mcpTitleInfo);
        if (err != 0) {
            LOG("MCP_UninstallTitleAsync failed for %s: %d", t.name.c_str(), (int)err);
            uninstallFailed++;
            uninstallCurrent++;
            return true;
        }
        uninstallInProgress = true;
        uninstallPollFrames = 0;
        return true;
    }

    MCPInstallProgress prog;
    memset(&prog, 0, sizeof(prog));
    MCP_InstallGetProgress(mcpHandle, &prog);
    uninstallPollFrames++;

    LOG("Poll[%d] '%s': inProgress=%u contentsTotal=%u",
        uninstallPollFrames, t.name.c_str(), prog.inProgress, prog.contentsTotal);

    if (prog.inProgress == 1) {
        uninstallSeenActive = true;
    } else if (prog.inProgress == 0) {
        if (uninstallSeenActive || uninstallPollFrames > 10) {
            LOG("Uninstall complete for '%s' (frames=%d seenActive=%d)",
                t.name.c_str(), uninstallPollFrames, (int)uninstallSeenActive);
            uninstallSucceeded++;
            uninstallInProgress  = false;
            uninstallSeenActive  = false;
            uninstallPollFrames  = 0;
            uninstallCurrent++;

            if (uninstallCurrent < (int)uninstallQueue.size()) {
                MCP_Close(mcpHandle);
                mcpHandle = MCP_Open();
                if (mcpHandle < 0) {
                    LOG("MCP_Open failed between uninstalls — aborting");
                    uninstallFailed += (int)uninstallQueue.size() - uninstallCurrent;
                    uninstallCurrent = (int)uninstallQueue.size();
                }
            }
        }
    }

    return true;
}

// input handling

void TitleUninstaller::Update(Input& input) {
    uint32_t now = Gfx::GetTicks();
    float dt = (lastTick > 0) ? (now - lastTick) / 1000.0f : 0.016f;
    lastTick = now;
    selectionPulse += dt * 3.0f;

    // Smooth scroll
    float diff = (float)targetScroll - scrollAnim;
    scrollAnim += diff * 0.18f;

    // Animate the green highlight bar toward the selected game's size fraction
    float targetFrac = 0.0f;
    if (usbTotalBytes > 0 && !titles.empty() &&
        selectedIndex >= 0 && selectedIndex < (int)titles.size()) {
        targetFrac = (float)titles[selectedIndex]->sizeBytes / (float)usbTotalBytes;
        if (targetFrac > 1.0f) targetFrac = 1.0f;
    }
    highlightBarAnim += (targetFrac - highlightBarAnim) * 0.12f;

    switch (state) {

    case AppState::Loading: {
        if (!loadingScreenShown) {
            loadingScreenShown = true;
        } else {
            OSReport("[UNINSTALLER] LoadTitles begin\n");
            LoadTitles();
            OSReport("[UNINSTALLER] LoadTitles done, %zu titles\n", titles.size());
            loadingScreenShown = false;
            state = AppState::List;
        }
        break;
    }

    // browse and select titles system
    case AppState::List: {
        LoadNextPendingIcon();

        int maxIdx = (int)titles.size() - 1;

        // Hold-repeat navigation: initial delay 0.3s, then fires every 0.12s,
        // accelerating to every 0.04s after holding for 1.5s.
        bool holdingDown = input.IsHeld(Input::BUTTON_DOWN);
        bool holdingUp   = input.IsHeld(Input::BUTTON_UP);
        bool holding     = holdingDown || holdingUp;

        int navDelta = 0;

        if (input.IsPressed(Input::BUTTON_DOWN) || input.IsPressed(Input::BUTTON_UP)) {
            // Fresh press — move immediately and reset hold timer
            navDelta   = input.IsPressed(Input::BUTTON_DOWN) ? 1 : -1;
            holdTimer  = 0.0f;
            repeatAccum = 0.0f;
        } else if (holding) {
            holdTimer += dt;
            if (holdTimer >= 0.3f) {
                // Repeat interval shrinks from 0.12s down to 0.04s after 1.5s hold
                float interval = (holdTimer < 1.5f) ? 0.12f : 0.04f;
                repeatAccum += dt;
                while (repeatAccum >= interval) {
                    repeatAccum -= interval;
                    navDelta += holdingDown ? 1 : -1;
                }
            }
        } else {
            holdTimer   = 0.0f;
            repeatAccum = 0.0f;
        }

        if (navDelta != 0) {
            auto moveSelection = [&](int delta) {
                if (delta > 0) {
                    if (selectedIndex < maxIdx) {
                        selectedIndex++;
                        if (selectedIndex >= targetScroll + VISIBLE_ROWS)
                            targetScroll = selectedIndex - VISIBLE_ROWS + 1;
                    } else {
                        selectedIndex = 0;
                        targetScroll  = 0;
                    }
                } else {
                    if (selectedIndex > 0) {
                        selectedIndex--;
                        if (selectedIndex < targetScroll)
                            targetScroll = selectedIndex;
                    } else {
                        selectedIndex = maxIdx;
                        targetScroll  = std::max(0, maxIdx - VISIBLE_ROWS + 1);
                    }
                }
            };
            // Clamp navDelta so it doesn't skip more than the list size
            int steps = std::min(std::abs(navDelta), (int)titles.size());
            int dir   = (navDelta > 0) ? 1 : -1;
            for (int s = 0; s < steps; s++) moveSelection(dir);
        }

        // A = toggle checkbox
        if (input.IsPressed(Input::BUTTON_A)) {
            if (!titles.empty())
                titles[selectedIndex]->checked = !titles[selectedIndex]->checked;
        }

        // Y = select all / deselect all
        if (input.IsPressed(Input::BUTTON_Y)) {
            bool anyUnchecked = false;
            for (const auto& t : titles) if (!t->checked) { anyUnchecked = true; break; }
            for (auto& t : titles) t->checked = anyUnchecked;
        }

        // + = confirm delete (only if something is checked)
        if (input.IsPressed(Input::BUTTON_PLUS)) {
            if (CheckedCount() > 0) state = AppState::ConfirmDelete;
        }

        // Minus = open settings
        if (input.IsPressed(Input::BUTTON_MINUS)) {
            settingsSelectedItem = 0;
            state = AppState::Settings;
        }

        // L/R = cycle sort mode
        if (input.IsPressed(Input::BUTTON_L) || input.IsPressed(Input::BUTTON_R)) {
            int next = (int)sortMode;
            if (input.IsPressed(Input::BUTTON_R))
                next = (next + 1) % (int)SortMode::COUNT;
            else
                next = (next - 1 + (int)SortMode::COUNT) % (int)SortMode::COUNT;
            sortMode = (SortMode)next;
            ApplySort();
            // Jump to top after re-sort
            selectedIndex = 0;
            targetScroll  = 0;
            scrollAnim    = 0.0f;
        }

        // ZL/ZR = switch storage location (USB ↔ NAND)
        if (input.IsPressed(Input::BUTTON_ZL) || input.IsPressed(Input::BUTTON_ZR)) {
            // Save current list back to its storage slot
            if (currentStorage == StorageLocation::USB)
                titlesUSB  = std::move(titles);
            else
                titlesNAND = std::move(titles);

            // Switch storage
            currentStorage = (currentStorage == StorageLocation::USB)
                             ? StorageLocation::NAND : StorageLocation::USB;

            // Restore the other list if already loaded, otherwise scan
            auto& otherList = (currentStorage == StorageLocation::USB)
                              ? titlesUSB : titlesNAND;

            if (!otherList.empty()) {
                // Already loaded — swap in instantly
                titles = std::move(otherList);
                selectedIndex = 0;
                targetScroll  = 0;
                scrollAnim    = 0.0f;
                usbTotalBytes = 0;
                usbFreeBytes  = 0;
                QueryStorage();
                // Reset icon loaded flags so icons reload for the new list
                for (auto& t : titles) {
                    if (t->icon) { SDL_DestroyTexture(t->icon); t->icon = nullptr; }
                    t->iconLoaded = false;
                }
                ApplySort();
            } else {
                titles.clear();
                selectedIndex      = 0;
                targetScroll       = 0;
                scrollAnim         = 0.0f;
                loadingScreenShown = false;
                usbTotalBytes      = 0;
                usbFreeBytes       = 0;
                state = AppState::Loading;
            }
        }
        break;
    }

    // Confirm dialog
    case AppState::ConfirmDelete: {
        // A = yes, proceed
        if (input.IsPressed(Input::BUTTON_A)) {
            StartUninstall();
        }
        // B = cancel, back to list view
        if (input.IsPressed(Input::BUTTON_B)) {
            state = AppState::List;
        }
        break;
    }

    // Uninstall commencing
    case AppState::Uninstalling: {
        UninstallNext();
        break;
    }

    // Uninstall Done 
    case AppState::Done: {
        // A = back to list
        if (input.IsPressed(Input::BUTTON_A)) {
            for (auto& t : titles) t->checked = false;
            selectedIndex      = std::min(selectedIndex, std::max(0, (int)titles.size() - 1));
            targetScroll       = std::min(targetScroll,  std::max(0, (int)titles.size() - 1));
            scrollAnim         = (float)targetScroll;
            state = AppState::List;
        }
        break;
    }

    // Settings Menu
    case AppState::Settings: {
        // Up/Down to navigate settings items
        if (input.IsPressed(Input::BUTTON_UP))
            settingsSelectedItem = std::max(0, settingsSelectedItem - 1);
        if (input.IsPressed(Input::BUTTON_DOWN))
            settingsSelectedItem = std::min(1, settingsSelectedItem + 1); // 2 items: Theme, (future expansion)

        // A = toggle/select
        if (input.IsPressed(Input::BUTTON_A)) {
            if (settingsSelectedItem == 0) {
                // Cycle theme
                int next = ((int)themeMode + 1) % (int)ThemeMode::COUNT;
                themeMode = (ThemeMode)next;
                if (themeMode == ThemeMode::Dark)
                    Gfx::SetTheme(Gfx::MakeDarkTheme());
                else
                    Gfx::SetTheme(Gfx::MakeLightTheme());
                SavePrefs();
            }
        }

        // B or Minus = back to list
        if (input.IsPressed(Input::BUTTON_B) || input.IsPressed(Input::BUTTON_MINUS)) {
            state = AppState::List;
        }
        break;
    }

    }
}

// Theme GFX Drawing

void TitleUninstaller::DrawBackground() {
    // Warm off-white gradient background
    Gfx::ClearGradient(Gfx::COLOR_BG_TOP(), Gfx::COLOR_BG_BOTTOM());
}

void TitleUninstaller::DrawTopBar() {
    // Solid amber top bar
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, 120, Gfx::COLOR_BAR_BG());
    // Subtle bottom shadow line
    Gfx::DrawRectFilled(0, 120, Gfx::SCREEN_WIDTH, 4, Gfx::COLOR_ACCENT_DARK());

    // App title in white
    Gfx::PrintWithShadow(60, 60, 52, Gfx::COLOR_WHITE, "Title Uninstaller",
                         Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    // Storage location tabs (USB / NAND)
    constexpr int TAB_W = 120, TAB_H = 36, TAB_Y = 60 - 18;
    int tabX = 60 + Gfx::GetTextWidth(52, "Title Uninstaller") + 40;

    SDL_Color usbBg  = (currentStorage == StorageLocation::USB)
                       ? Gfx::COLOR_WHITE : Gfx::COLOR_ACCENT_DARK();
    SDL_Color usbTxt = (currentStorage == StorageLocation::USB)
                       ? Gfx::COLOR_ACCENT_DARK() : Gfx::COLOR_WHITE;
    Gfx::DrawRectRounded(tabX, TAB_Y, TAB_W, TAB_H, 8, usbBg);
    Gfx::Print(tabX + TAB_W / 2, 60, 24, usbTxt, "USB",
               Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_VERTICAL);

    SDL_Color nandBg  = (currentStorage == StorageLocation::NAND)
                        ? Gfx::COLOR_WHITE : Gfx::COLOR_ACCENT_DARK();
    SDL_Color nandTxt = (currentStorage == StorageLocation::NAND)
                        ? Gfx::COLOR_ACCENT_DARK() : Gfx::COLOR_WHITE;
    Gfx::DrawRectRounded(tabX + TAB_W + 8, TAB_Y, TAB_W, TAB_H, 8, nandBg);
    Gfx::Print(tabX + TAB_W + 8 + TAB_W / 2, 60, 24, nandTxt, "NAND",
               Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_VERTICAL);

    // "ZL/ZR Switch Storage" hint immediately to the right of the NAND tab
    int hintX = tabX + TAB_W + 8 + TAB_W + 20;
    int iw1 = Gfx::GetIconTextWidth(28, "\xee\x82\x85"); // ZL
    int iw2 = Gfx::GetIconTextWidth(28, "\xee\x82\x86"); // ZR
    int tw  = Gfx::GetTextWidth(22, "Switch");
    constexpr int HINT_GAP = 4;
    Gfx::PrintIcon(hintX, 60, 28, Gfx::COLOR_ACCENT_DARK(),
                   "\xee\x82\x85", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::PrintIcon(hintX + iw1 + 2, 60, 28, Gfx::COLOR_ACCENT_DARK(),
                   "\xee\x82\x86", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::Print(hintX + iw1 + 2 + iw2 + HINT_GAP, 60, 22, Gfx::COLOR_WHITE,
               "Switch", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    // Sort mode pill (top right)
    const char* sortLabel = "A\xe2\x80\x93Z";
    if (sortMode == SortMode::SizeDesc) sortLabel = "Size \xe2\x86\x93";
    if (sortMode == SortMode::SizeAsc)  sortLabel = "Size \xe2\x86\x91";
    std::string sortStr = std::string("Sort: ") + sortLabel;
    int sw = Gfx::GetTextWidth(26, sortStr);
    constexpr int PILL_PAD = 20;  // horizontal padding inside pill
    constexpr int PILL_H   = 36;
    int pillX = Gfx::SCREEN_WIDTH - 60 - sw - PILL_PAD;
    int pillY = 60 - PILL_H / 2;
    Gfx::DrawRectRounded(pillX, pillY, sw + PILL_PAD * 2, PILL_H, 10,
                         Gfx::COLOR_ACCENT_DARK());
    // Text centred inside the pill
    Gfx::Print(pillX + PILL_PAD + sw / 2, 60, 26, Gfx::COLOR_WHITE,
               sortStr, Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_VERTICAL);
}

void TitleUninstaller::DrawBottomBar() {
    // White bottom bar with top border
    Gfx::DrawRectFilled(0, 1000, Gfx::SCREEN_WIDTH, 80, Gfx::COLOR_BAR_BOTTOM());
    Gfx::DrawRectFilled(0, 1000, Gfx::SCREEN_WIDTH, 3, Gfx::COLOR_ACCENT());

    constexpr int ICON_SZ = 34;
    constexpr int TXT_SZ  = 26;
    constexpr int GAP     = 6;
    constexpr int Y       = 1040;

    // Dark text on white bar
    auto drawHint = [&](int x, const char* glyph, const char* label, Gfx::AlignFlags align) {
        int iw = Gfx::GetIconTextWidth(ICON_SZ, glyph);
        int tw = Gfx::GetTextWidth(TXT_SZ, label);
        int total = iw + GAP + tw;
        int sx = x;
        if (align & Gfx::ALIGN_HORIZONTAL) sx = x - total / 2;
        else if (align & Gfx::ALIGN_RIGHT) sx = x - total;
        Gfx::PrintIcon(sx, Y, ICON_SZ, Gfx::COLOR_ACCENT_DARK(), glyph,
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(sx + iw + GAP, Y, TXT_SZ, Gfx::COLOR_TEXT(), label,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    };

    // Far left: Selected count pill (original position)
    int checked = CheckedCount();
    int total   = (int)titles.size();
    char countBuf[64];
    snprintf(countBuf, sizeof(countBuf), "Selected %d / %d", checked, total);
    int cw2 = Gfx::GetTextWidth(TXT_SZ, countBuf);
    Gfx::DrawRectRounded(40, Y - 16, cw2 + 24, 32, 8, Gfx::COLOR_ACCENT());
    Gfx::Print(52, Y, TXT_SZ, Gfx::COLOR_WHITE, countBuf,
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    // Settings hint immediately to the right of the count pill
    int settingsX = 40 + cw2 + 24 + 20;
    drawHint(settingsX, "\xee\x81\x86", "Settings", Gfx::ALIGN_LEFT);

    // Centre: Exit
    drawHint(Gfx::SCREEN_WIDTH / 2, "\xee\x81\x84", "Exit", Gfx::ALIGN_HORIZONTAL);

    // Left of centre: Sort
    drawHint(Gfx::SCREEN_WIDTH / 2 - 160, "\xee\x82\x83\xee\x82\x84", "Sort",
             Gfx::ALIGN_RIGHT);

    // Right: action hints
    drawHint(1920 - 40,       "\xee\x80\x80", "Select",          Gfx::ALIGN_RIGHT);
    drawHint(1920 - 40 - 220, "\xee\x80\x83", "All/None",        Gfx::ALIGN_RIGHT);
    drawHint(1920 - 40 - 440, "\xee\x81\x85", "Delete Selected", Gfx::ALIGN_RIGHT);
}

// List view

void TitleUninstaller::DrawList() {
    if (titles.empty()) {
        Gfx::DrawRectRounded(LIST_X, LIST_Y, LIST_W, 400, 16, Gfx::COLOR_PANEL_BG());
        Gfx::DrawRectOutline(LIST_X, LIST_Y, LIST_W, 400, Gfx::COLOR_SEPARATOR(), 2);
        Gfx::Print(LIST_X + LIST_W / 2, LIST_Y + 200, 36, Gfx::COLOR_TEXT_DIM(),
                   (currentStorage == StorageLocation::USB)
                   ? "No USB titles found" : "No NAND titles found",
                   Gfx::ALIGN_CENTER);
        return;
    }

    constexpr int LIST_BOTTOM = 1000;
    SDL_Rect clip = { LIST_X, LIST_Y, LIST_W + 20, LIST_BOTTOM - LIST_Y };
    SDL_RenderSetClipRect(Gfx::GetRenderer(), &clip);

    int firstRow = (int)scrollAnim;
    float subOffset = (scrollAnim - firstRow) * ROW_H;

    for (int row = 0; row < VISIBLE_ROWS + 1; row++) {
        int idx = firstRow + row;
        if (idx < 0 || idx >= (int)titles.size()) continue;

        int x = LIST_X;
        int y = LIST_Y + row * ROW_H - (int)subOffset;

        if (y + ROW_H < LIST_Y || y > LIST_BOTTOM) continue;

        TitleEntry& t = *titles[idx];
        bool sel = (idx == selectedIndex);

        // Card shadow
        Gfx::DrawRectFilled(x + 3, y + 3, LIST_W - 6, ROW_H - 6,
                            {0x00, 0x00, 0x00, 0x18});

        // Card background
        SDL_Color rowBg = sel ? Gfx::COLOR_ROW_SELECTED()
                              : (t.checked ? Gfx::COLOR_ROW_CHECKED() : Gfx::COLOR_ROW_BG());
        Gfx::DrawRectRounded(x + 2, y + 2, LIST_W - 4, ROW_H - 6, 10, rowBg);

        // Selected: amber border + left accent bar
        if (sel) {
            float pulse = std::sin(selectionPulse) * 0.3f + 0.7f;
            uint8_t alpha = (uint8_t)(180 * pulse + 75);
            SDL_Color border = {Gfx::COLOR_ACCENT().r, Gfx::COLOR_ACCENT().g,
                                Gfx::COLOR_ACCENT().b, alpha};
            Gfx::DrawRectOutline(x + 2, y + 2, LIST_W - 4, ROW_H - 6, border, 3);
            Gfx::DrawRectFilled(x + 2, y + 2, 6, ROW_H - 6, Gfx::COLOR_ACCENT());
        }

        // Circular checkbox
        int cbX = x + 36;
        int cbY = y + ROW_H / 2;
        int cbR = 14;
        if (t.checked) {
            Gfx::DrawCircleFilled(cbX, cbY, cbR, Gfx::COLOR_ACCENT());
            Gfx::DrawCircleFilled(cbX, cbY, cbR - 5, Gfx::COLOR_WHITE);
        } else {
            SDL_Color cbBg = sel ? Gfx::COLOR_ACCENT_LIGHT()
                                 : Gfx::COLOR_SEPARATOR();
            Gfx::DrawCircleFilled(cbX, cbY, cbR, cbBg);
            Gfx::DrawCircleFilled(cbX, cbY, cbR - 3, rowBg);
        }

        // Icon
        int iconX = cbX + cbR + 20;
        int iconY = y + (ROW_H - ICON_SIZE) / 2;
        if (t.icon) {
            Gfx::DrawRectRounded(iconX - 2, iconY - 2, ICON_SIZE + 4, ICON_SIZE + 4,
                                 8, Gfx::COLOR_SEPARATOR());
            Gfx::DrawTexture(t.icon, iconX, iconY, ICON_SIZE, ICON_SIZE);
        } else {
            Gfx::DrawRectRounded(iconX, iconY, ICON_SIZE, ICON_SIZE, 8,
                                 Gfx::COLOR_SEPARATOR());
            Gfx::Print(iconX + ICON_SIZE / 2, iconY + ICON_SIZE / 2, 32,
                       Gfx::COLOR_ACCENT_DARK(), "?", Gfx::ALIGN_CENTER);
        }

        // Title name — dark text
        int textX = iconX + ICON_SIZE + 20;
        Gfx::Print(textX, y + ROW_H / 2, 32, Gfx::COLOR_TEXT(), t.name,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

        // Size — amber, right-aligned
        std::string sizeStr = FormatSize(t.sizeBytes);
        Gfx::Print(LIST_X + LIST_W - 24, y + ROW_H / 2, 30,
                   Gfx::COLOR_SIZE_TEXT(), sizeStr,
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    }

    // Scroll indicator — amber thumb
    if ((int)titles.size() > VISIBLE_ROWS) {
        int trackH = VISIBLE_ROWS * ROW_H;
        int trackX = LIST_X + LIST_W + 10;
        int trackY = LIST_Y;
        Gfx::DrawRectRounded(trackX, trackY, 8, trackH, 4,
                             {0xe0, 0xdc, 0xd0, 0xff});
        float ratio = (float)targetScroll / std::max(1, (int)titles.size() - VISIBLE_ROWS);
        int thumbH  = std::max(40, trackH * VISIBLE_ROWS / (int)titles.size());
        int thumbY  = trackY + (int)(ratio * (trackH - thumbH));
        Gfx::DrawRectRounded(trackX, thumbY, 8, thumbH, 4, Gfx::COLOR_ACCENT());
    }

    SDL_RenderSetClipRect(Gfx::GetRenderer(), nullptr);
}

// Storage panel

void TitleUninstaller::DrawStoragePanel() {
    int px = PANEL_X;
    int py = PANEL_Y;
    int pw = PANEL_W;

    // White card with subtle shadow
    Gfx::DrawRectFilled(px + 4, py + 4, pw, 360, {0x00, 0x00, 0x00, 0x18});
    Gfx::DrawRectRounded(px, py, pw, 360, 14, Gfx::COLOR_PANEL_BG());
    Gfx::DrawRectOutline(px, py, pw, 360, Gfx::COLOR_SEPARATOR(), 1);

    // Amber header strip
    Gfx::DrawRectRounded(px, py, pw, 52, 14, Gfx::COLOR_ACCENT());
    Gfx::DrawRectFilled(px, py + 38, pw, 14, Gfx::COLOR_ACCENT()); // square bottom corners
    Gfx::Print(px + 20, py + 26, 26, Gfx::COLOR_WHITE,
               (currentStorage == StorageLocation::USB) ? "USB Storage" : "NAND Storage",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    // Storage bar
    int barX = px + 20;
    int barY = py + 80;
    int barW = pw - 40;
    int barH = 20;
    // Rounded bar track
    Gfx::DrawRectRounded(barX, barY, barW, barH, barH / 2, Gfx::COLOR_STORAGE_FREE());

    int usedW = 0;
    if (usbTotalBytes > 0) {
        usedW = (int)((float)barW * (1.0f - (float)usbFreeBytes / usbTotalBytes));
        if (usedW > 0)
            Gfx::DrawRectRounded(barX, barY, usedW, barH, barH / 2, Gfx::COLOR_STORAGE_USED());
    } else {
        usedW = barW / 2;
        Gfx::DrawRectRounded(barX, barY, usedW, barH, barH / 2, Gfx::COLOR_STORAGE_USED());
    }

    // Highlighted game shows a blinking green overlay at the right edge of used
    // area, representing how much space would be freed by uninstalling it.
    // highlightBarAnim smoothly tracks the selected game's size fraction.
    if (usbTotalBytes > 0 && highlightBarAnim > 0.001f) {
        int freeW = (int)(highlightBarAnim * barW);
        freeW = std::max(4, std::min(freeW, barW - usedW));
        float blink = std::sin(selectionPulse * 2.0f) * 0.5f + 0.5f;
        uint8_t alpha = (uint8_t)(120 + blink * 135);
        SDL_Color green = {0x44, 0xff, 0x88, alpha};
        Gfx::DrawRectFilled(barX + usedW, barY, freeW, barH, green);
    }

    // Space available
    Gfx::Print(px + 20, barY + barH + 12, 26, Gfx::COLOR_TEXT_DIM(), "Space available",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);
    std::string freeStr = (usbFreeBytes > 0) ? FormatSize(usbFreeBytes) : "N/A";
    Gfx::Print(px + pw - 20, barY + barH + 12, 26, Gfx::COLOR_TEXT(), freeStr,
               Gfx::ALIGN_RIGHT | Gfx::ALIGN_TOP);

    // Separator
    Gfx::DrawRectFilled(px + 20, py + 150, pw - 40, 1, Gfx::COLOR_SEPARATOR());

    // Selection summary
    int checked = CheckedCount();
    uint64_t selBytes = CheckedBytes();

    Gfx::Print(px + 20, py + 170, 26, Gfx::COLOR_TEXT_DIM(), "Selected for deletion",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);
    char selBuf[32];
    snprintf(selBuf, sizeof(selBuf), "%d title%s", checked, checked == 1 ? "" : "s");
    Gfx::Print(px + 20, py + 206, 34, Gfx::COLOR_TEXT(), selBuf,
               Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);

    if (checked > 0) {
        Gfx::Print(px + 20, py + 254, 34, Gfx::COLOR_DANGER(),
                   FormatSize(selBytes), Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);
        Gfx::Print(px + 20 + Gfx::GetTextWidth(34, FormatSize(selBytes)) + 10,
                   py + 254, 24, Gfx::COLOR_TEXT_DIM(), "will be freed",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);
    }

    // Total titles count
    Gfx::DrawRectFilled(px + 20, py + 305, pw - 40, 1, Gfx::COLOR_SEPARATOR());
    char totalBuf[48];
    snprintf(totalBuf, sizeof(totalBuf), "%d %s title%s installed",
             (int)titles.size(),
             (currentStorage == StorageLocation::USB) ? "USB" : "NAND",
             titles.size() == 1 ? "" : "s");
    Gfx::Print(px + 20, py + 320, 24, Gfx::COLOR_TEXT_DIM(), totalBuf,
               Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);
}

// Confirm dialog

void TitleUninstaller::DrawConfirmDialog() {
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT, {0x00, 0x00, 0x00, 0xb0});

    int dw = 860, dh = 360;
    int dx = (Gfx::SCREEN_WIDTH  - dw) / 2;
    int dy = (Gfx::SCREEN_HEIGHT - dh) / 2;

    // White card with shadow
    Gfx::DrawRectFilled(dx + 5, dy + 5, dw, dh, {0x00, 0x00, 0x00, 0x30});
    Gfx::DrawRectRounded(dx, dy, dw, dh, 20, Gfx::COLOR_PANEL_BG());

    // Amber header strip
    Gfx::DrawRectRounded(dx, dy, dw, 60, 20, Gfx::COLOR_DANGER());
    Gfx::DrawRectFilled(dx, dy + 40, dw, 20, Gfx::COLOR_DANGER());
    Gfx::Print(dx + dw / 2, dy + 30, 36, Gfx::COLOR_WHITE,
               "Confirm Deletion", Gfx::ALIGN_CENTER | Gfx::ALIGN_VERTICAL);

    int checked = CheckedCount();
    char line1[80];
    snprintf(line1, sizeof(line1), "Delete %d title%s?", checked, checked == 1 ? "" : "s");
    Gfx::Print(dx + dw / 2, dy + 110, 34, Gfx::COLOR_TEXT(),
               line1, Gfx::ALIGN_CENTER | Gfx::ALIGN_TOP);

    std::string sizeStr = "This will free " + FormatSize(CheckedBytes()) + " of storage.";
    Gfx::Print(dx + dw / 2, dy + 160, 28, Gfx::COLOR_TEXT_DIM(),
               sizeStr, Gfx::ALIGN_CENTER | Gfx::ALIGN_TOP);

    Gfx::Print(dx + dw / 2, dy + 205, 26, Gfx::COLOR_DANGER(),
               "This action cannot be undone!", Gfx::ALIGN_CENTER | Gfx::ALIGN_TOP);

    int btnY = dy + dh - 80;
    constexpr int ICON_SZ = 30;
    constexpr int TXT_SZ  = 26;
    constexpr int GAP     = 6;

    // A = Confirm (red button)
    Gfx::DrawRectRounded(dx + 80, btnY, 280, 52, 12, Gfx::COLOR_DANGER());
    {
        int iw = Gfx::GetIconTextWidth(ICON_SZ, "\xee\x80\x80");
        int tw = Gfx::GetTextWidth(TXT_SZ, "Confirm");
        int total = iw + GAP + tw;
        int sx = dx + 80 + 140 - total / 2;
        Gfx::PrintIcon(sx, btnY + 26, ICON_SZ, Gfx::COLOR_WHITE,
                       "\xee\x80\x80", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(sx + iw + GAP, btnY + 26, TXT_SZ, Gfx::COLOR_WHITE,
                   "Confirm", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }

    // B = Cancel (grey button)
    Gfx::DrawRectRounded(dx + dw - 360, btnY, 280, 52, 12, Gfx::COLOR_SEPARATOR());
    {
        int iw = Gfx::GetIconTextWidth(ICON_SZ, "\xee\x80\x81");
        int tw = Gfx::GetTextWidth(TXT_SZ, "Cancel");
        int total = iw + GAP + tw;
        int sx = dx + dw - 360 + 140 - total / 2;
        Gfx::PrintIcon(sx, btnY + 26, ICON_SZ, Gfx::COLOR_TEXT(),
                       "\xee\x80\x81", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(sx + iw + GAP, btnY + 26, TXT_SZ, Gfx::COLOR_TEXT(),
                   "Cancel", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }
}

// Uninstall progress screen

void TitleUninstaller::DrawUninstallProgress() {
    DrawBackground();
    DrawTopBar();

    int total   = (int)uninstallQueue.size();
    int current = std::min(uninstallCurrent, total);

    // White card
    Gfx::DrawRectFilled(203, 383, 1520, 314, {0x00, 0x00, 0x00, 0x18});
    Gfx::DrawRectRounded(200, 380, 1520, 314, 16, Gfx::COLOR_PANEL_BG());

    // Current title name
    if (uninstallCurrent < total) {
        int idx = uninstallQueue[uninstallCurrent];
        Gfx::Print(960, 430, 34, Gfx::COLOR_TEXT(),
                   "Uninstalling: " + titles[idx]->name, Gfx::ALIGN_CENTER);
    }

    // Rounded progress bar
    int barX = 260, barY = 490, barW = 1400, barH = 24;
    Gfx::DrawRectRounded(barX, barY, barW, barH, barH / 2, Gfx::COLOR_STORAGE_FREE());
    if (total > 0) {
        int fillW = (int)((float)barW * current / total);
        if (fillW > 0)
            Gfx::DrawRectRounded(barX, barY, fillW, barH, barH / 2, Gfx::COLOR_ACCENT());
    }

    // Counter
    char buf[64];
    snprintf(buf, sizeof(buf), "%d / %d", current, total);
    Gfx::Print(960, 540, 28, Gfx::COLOR_TEXT_DIM(), buf, Gfx::ALIGN_CENTER);

    if (uninstallSucceeded > 0 || uninstallFailed > 0) {
        char stats[80];
        snprintf(stats, sizeof(stats), "Done: %d   Failed: %d",
                 uninstallSucceeded, uninstallFailed);
        Gfx::Print(960, 580, 24, Gfx::COLOR_TEXT_DIM(), stats, Gfx::ALIGN_CENTER);
    }
}

// Done screen

void TitleUninstaller::DrawDoneScreen() {
    DrawBackground();
    DrawTopBar();

    // White card with shadow
    Gfx::DrawRectFilled(363, 283, 1200, 420, {0x00, 0x00, 0x00, 0x18});
    Gfx::DrawRectRounded(360, 280, 1200, 420, 20, Gfx::COLOR_PANEL_BG());

    // Amber header strip
    Gfx::DrawRectRounded(360, 280, 1200, 60, 20, Gfx::COLOR_ACCENT());
    Gfx::DrawRectFilled(360, 320, 1200, 20, Gfx::COLOR_ACCENT());
    Gfx::Print(960, 310, 36, Gfx::COLOR_WHITE, "Uninstall Complete",
               Gfx::ALIGN_CENTER | Gfx::ALIGN_VERTICAL);

    char line[80];
    snprintf(line, sizeof(line), "%d title%s removed successfully.",
             uninstallSucceeded, uninstallSucceeded == 1 ? "" : "s");
    Gfx::Print(960, 410, 34, Gfx::COLOR_TEXT(), line, Gfx::ALIGN_CENTER);

    if (uninstallFailed > 0) {
        char fail[80];
        snprintf(fail, sizeof(fail), "%d title%s failed to uninstall.",
                 uninstallFailed, uninstallFailed == 1 ? "" : "s");
        Gfx::Print(960, 460, 28, Gfx::COLOR_DANGER(), fail, Gfx::ALIGN_CENTER);
    }

    // Hints
    {
        constexpr int ICON_SZ = 30;
        constexpr int TXT_SZ  = 26;
        constexpr int GAP     = 6;
        constexpr int Y       = 560;

        int iw1 = Gfx::GetIconTextWidth(ICON_SZ, "\xee\x80\x80");
        int tw1 = Gfx::GetTextWidth(TXT_SZ, "Back to list");
        int iw2 = Gfx::GetIconTextWidth(ICON_SZ, "\xee\x81\x84");
        int tw2 = Gfx::GetTextWidth(TXT_SZ, "Exit (HOME)");
        constexpr int SEP = 60;
        int total = iw1 + GAP + tw1 + SEP + iw2 + GAP + tw2;
        int sx = 960 - total / 2;

        Gfx::PrintIcon(sx, Y, ICON_SZ, Gfx::COLOR_ACCENT_DARK(),
                       "\xee\x80\x80", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(sx + iw1 + GAP, Y, TXT_SZ, Gfx::COLOR_TEXT(),
                   "Back to list", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

        int sx2 = sx + iw1 + GAP + tw1 + SEP;
        Gfx::PrintIcon(sx2, Y, ICON_SZ, Gfx::COLOR_ACCENT_DARK(),
                       "\xee\x81\x84", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(sx2 + iw2 + GAP, Y, TXT_SZ, Gfx::COLOR_TEXT(),
                   "Exit (HOME)", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }
}

// Loading screen

void TitleUninstaller::DrawLoadingScreen() {
    DrawBackground();
    DrawTopBar();

    // Amber pulsing card
    float pulse = std::sin(Gfx::GetTicks() / 400.0f) * 0.15f + 0.85f;
    SDL_Color cardColor = {
        (uint8_t)(Gfx::COLOR_ACCENT().r * pulse),
        (uint8_t)(Gfx::COLOR_ACCENT().g * pulse),
        (uint8_t)(Gfx::COLOR_ACCENT().b * pulse), 0xff
    };

    Gfx::DrawRectFilled(563, 453, 800, 174, {0x00, 0x00, 0x00, 0x18});
    Gfx::DrawRectRounded(560, 450, 800, 174, 20, Gfx::COLOR_PANEL_BG());
    Gfx::DrawRectRounded(560, 450, 800, 52, 20, cardColor);
    Gfx::DrawRectFilled(560, 482, 800, 20, cardColor);
    Gfx::Print(960, 476, 30, Gfx::COLOR_WHITE,
               (currentStorage == StorageLocation::USB)
               ? "Scanning USB storage..." : "Scanning NAND storage...",
               Gfx::ALIGN_CENTER | Gfx::ALIGN_VERTICAL);
    Gfx::Print(960, 560, 26, Gfx::COLOR_TEXT_DIM(), "Please wait", Gfx::ALIGN_CENTER);
}

// Settings screen

void TitleUninstaller::DrawSettingsScreen() {
    DrawBackground();

    // Top bar — same amber bar, subtitle shows "Settings"
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, 120, Gfx::COLOR_BAR_BG());
    Gfx::DrawRectFilled(0, 120, Gfx::SCREEN_WIDTH, 4, Gfx::COLOR_ACCENT_DARK());
    Gfx::PrintWithShadow(60, 60, 52, Gfx::COLOR_WHITE, "Title Uninstaller",
                         Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    // Subtitle
    int titleW = Gfx::GetTextWidth(52, "Title Uninstaller");
    Gfx::Print(60 + titleW + 24, 60, 28, Gfx::COLOR_ACCENT_DARK(), "/ Settings",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    // Card (no header strip — title is in the top bar)
    int cw = 860, ch = 360;
    int cx = (Gfx::SCREEN_WIDTH  - cw) / 2;
    int cy = (Gfx::SCREEN_HEIGHT - ch) / 2;

    Gfx::DrawRectFilled(cx + 4, cy + 4, cw, ch, {0x00, 0x00, 0x00, 0x30});
    Gfx::DrawRectRounded(cx, cy, cw, ch, 20, Gfx::COLOR_PANEL_BG());
    Gfx::DrawRectOutline(cx, cy, cw, ch, Gfx::COLOR_SEPARATOR(), 1);

    // Theme row
    constexpr int ROW_Y1  = 40;
    constexpr int ROW_H_S = 90;

    auto drawSettingRow = [&](int rowY, int itemIdx, const char* label,
                               const char* valueLabel) {
        bool sel = (settingsSelectedItem == itemIdx);
        SDL_Color rowBg = sel ? Gfx::COLOR_ROW_SELECTED() : Gfx::COLOR_ROW_BG();
        Gfx::DrawRectRounded(cx + 20, cy + rowY, cw - 40, ROW_H_S, 12, rowBg);
        if (sel) {
            float pulse = std::sin(selectionPulse) * 0.3f + 0.7f;
            uint8_t alpha = (uint8_t)(180 * pulse + 75);
            SDL_Color border = {Gfx::COLOR_ACCENT().r, Gfx::COLOR_ACCENT().g,
                                Gfx::COLOR_ACCENT().b, alpha};
            Gfx::DrawRectOutline(cx + 20, cy + rowY, cw - 40, ROW_H_S, border, 3);
            Gfx::DrawRectFilled(cx + 20, cy + rowY, 6, ROW_H_S, Gfx::COLOR_ACCENT());
        }
        Gfx::Print(cx + 50, cy + rowY + ROW_H_S / 2, 30, Gfx::COLOR_TEXT(),
                   label, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        int vw = Gfx::GetTextWidth(26, valueLabel) + 24;
        Gfx::DrawRectRounded(cx + cw - 40 - vw, cy + rowY + ROW_H_S / 2 - 18,
                             vw, 36, 10, Gfx::COLOR_ACCENT());
        Gfx::Print(cx + cw - 40 - vw / 2, cy + rowY + ROW_H_S / 2, 26,
                   Gfx::COLOR_WHITE, valueLabel, Gfx::ALIGN_CENTER | Gfx::ALIGN_VERTICAL);
    };

    const char* themeLabel = (themeMode == ThemeMode::Dark) ? "Dark" : "Light";
    drawSettingRow(ROW_Y1, 0, "Theme", themeLabel);

    // In-card hints
    {
        constexpr int HINT_SZ = 26, HINT_ICON = 28, HINT_GAP = 5;
        int hy = cy + ROW_Y1 + ROW_H_S + 50;
        int iw1 = Gfx::GetIconTextWidth(HINT_ICON, "\xee\x80\x80");
        int tw1 = Gfx::GetTextWidth(HINT_SZ, "Change");
        int iw2 = Gfx::GetIconTextWidth(HINT_ICON, "\xee\x80\x81");
        int tw2 = Gfx::GetTextWidth(HINT_SZ, "Back");
        int totalW = iw1 + HINT_GAP + tw1 + 50 + iw2 + HINT_GAP + tw2;
        int sx = cx + cw / 2 - totalW / 2;
        Gfx::PrintIcon(sx, hy, HINT_ICON, Gfx::COLOR_ACCENT_DARK(),
                       "\xee\x80\x80", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(sx + iw1 + HINT_GAP, hy, HINT_SZ, Gfx::COLOR_TEXT_DIM(),
                   "Change", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        int sx2 = sx + iw1 + HINT_GAP + tw1 + 50;
        Gfx::PrintIcon(sx2, hy, HINT_ICON, Gfx::COLOR_ACCENT_DARK(),
                       "\xee\x80\x81", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(sx2 + iw2 + HINT_GAP, hy, HINT_SZ, Gfx::COLOR_TEXT_DIM(),
                   "Back", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }

    Gfx::DrawRectFilled(0, 1000, Gfx::SCREEN_WIDTH, 80, Gfx::COLOR_BAR_BOTTOM());
    Gfx::DrawRectFilled(0, 1000, Gfx::SCREEN_WIDTH, 3, Gfx::COLOR_ACCENT());
    constexpr int Y = 1040;
    constexpr int ICON_SZ = 34, TXT_SZ = 26, GAP = 6;
    auto drawHint = [&](int x, const char* glyph, const char* lbl, Gfx::AlignFlags align) {
        int iw = Gfx::GetIconTextWidth(ICON_SZ, glyph);
        int tw = Gfx::GetTextWidth(TXT_SZ, lbl);
        int sx = x;
        if (align & Gfx::ALIGN_HORIZONTAL) sx = x - (iw + GAP + tw) / 2;
        else if (align & Gfx::ALIGN_RIGHT)  sx = x - (iw + GAP + tw);
        Gfx::PrintIcon(sx, Y, ICON_SZ, Gfx::COLOR_ACCENT_DARK(), glyph,
                       Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(sx + iw + GAP, Y, TXT_SZ, Gfx::COLOR_TEXT(), lbl,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    };
    drawHint(Gfx::SCREEN_WIDTH / 2, "\xee\x81\x84", "Exit (HOME)", Gfx::ALIGN_HORIZONTAL);
    drawHint(1920 - 40,       "\xee\x80\x80", "Change", Gfx::ALIGN_RIGHT);
    drawHint(1920 - 40 - 200, "\xee\x80\x81", "Back",   Gfx::ALIGN_RIGHT);
}

// Main Draw dispatcher

void TitleUninstaller::Draw() {
    switch (state) {
    case AppState::Loading:
        DrawLoadingScreen();
        break;

    case AppState::List:
        DrawBackground();
        DrawTopBar();
        DrawList();
        DrawStoragePanel();
        DrawBottomBar();
        break;

    case AppState::ConfirmDelete:
        DrawBackground();
        DrawTopBar();
        DrawList();
        DrawStoragePanel();
        DrawBottomBar();
        DrawConfirmDialog();
        break;

    case AppState::Uninstalling:
        DrawUninstallProgress();
        break;

    case AppState::Done:
        DrawDoneScreen();
        break;

    case AppState::Settings:
        DrawSettingsScreen();
        break;
    }
}
