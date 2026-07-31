#include "TitleUninstaller.hpp"
#include "TitleDefs.hpp"
#include <coreinit/filesystem.h>
#include <coreinit/mcp.h>
#include <nn/acp.h>
#include <whb/proc.h>
#include <dirent.h>
#include <sys/stat.h>
#include <malloc.h>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

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

static uint64_t ReadTmdContentSize(const std::string& tmdPath) {
    FILE* f = fopen(tmdPath.c_str(), "rb");
    if (!f) return 0;

    uint64_t total = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long fileSize = ftell(f);
        if (fileSize >= 0x1E4 + 0x30) {
            std::vector<uint8_t> hdr(0x1E4);
            fseek(f, 0, SEEK_SET);
            if (fread(hdr.data(), 1, hdr.size(), f) == hdr.size()) {
                uint8_t  version     = hdr[0x180];
                uint16_t numContents = 0;
                memcpy(&numContents, &hdr[0x1DE], sizeof(numContents));

                uint32_t contentsOff = (version == 1) ? 0x0B04 : 0x1E4;
                size_t   recBytes    = (size_t)numContents * 0x30;

                if (numContents > 0 && numContents <= 0x1000 &&
                    (uint64_t)contentsOff + recBytes <= (uint64_t)fileSize) {
                    std::vector<uint8_t> buf(recBytes);
                    fseek(f, contentsOff, SEEK_SET);
                    if (fread(buf.data(), 1, buf.size(), f) == buf.size()) {
                        for (size_t i = 0; i < (size_t)numContents; i++) {
                            uint64_t sz;
                            memcpy(&sz, &buf[i * 0x30 + 0x08], sizeof(sz));
                            total += sz;
                        }
                    }
                }
            }
        }
    }
    fclose(f);
    return total;
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
            const MCPTitleListType& info = list[i];
            if (info.appType != MCP_APP_TYPE_GAME && info.appType != MCP_APP_TYPE_GAME_WII) continue;
            if (!TitleId::IsBaseGame(info.titleId)) continue;
            if (forUSB  && TitleOnUSB(info))  count++;
            if (!forUSB && TitleOnNAND(info)) count++;
        }
    }

    MCP_Close(handle);
    return count;
}

void TitleUninstaller::SaveTitleCache() {
    EnsureCacheDir();
    const char* path = (currentStorage == StorageLocation::USB)
                       ? CachePath::USB() : CachePath::NAND();
    FILE* f = fopen(path, "w");
    if (!f) { LOG("SaveTitleCache: fopen failed"); return; }

    int rawCount = 0;
    {
        int32_t h = MCP_Open();
        if (h >= 0) {
            uint32_t total = MCP_TitleCount(h);
            if (total > 0) {
                std::vector<MCPTitleListType> list(total);
                uint32_t fetched = total;
                MCP_TitleList(h, &fetched, list.data(), fetched * sizeof(MCPTitleListType));
                for (uint32_t i = 0; i < fetched; i++) {
                    const MCPTitleListType& info = list[i];
                    if (info.appType != MCP_APP_TYPE_GAME && info.appType != MCP_APP_TYPE_GAME_WII) continue;
                    if (!TitleId::IsBaseGame(info.titleId)) continue;
                    if (currentStorage == StorageLocation::USB  && TitleOnUSB(info))  rawCount++;
                    if (currentStorage == StorageLocation::NAND && TitleOnNAND(info)) rawCount++;
                }
            }
            MCP_Close(h);
        }
    }
    fprintf(f, "#count=%d;v=7\n", rawCount);

    for (const auto& t : titles) {
        std::string safeName = t->name;
        for (char& c : safeName) if (c == '|') c = ' ';
        fprintf(f, "%016" PRIx64 "|%s|%" PRIu64 "|%s\n",
                t->titleId, t->path.c_str(), t->sizeBytes, safeName.c_str());
    }
    fclose(f);
    LOG("SaveTitleCache: wrote %zu entries", titles.size());
}

bool TitleUninstaller::LoadTitleCache() {
    const char* path = (currentStorage == StorageLocation::USB)
                       ? CachePath::USB() : CachePath::NAND();
    FILE* f = fopen(path, "r");
    if (!f) return false;

    char header[64] = {};
    if (!fgets(header, sizeof(header), f)) { fclose(f); return false; }

    int cachedCount = 0;
    int ver = 0;
    if (sscanf(header, "#count=%d;v=%d", &cachedCount, &ver) != 2 || ver != 7) {
        fclose(f);
        remove(path);
        return false;
    }

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

        // Reject cached entries that aren't base games
        if (!TitleId::IsBaseGame(titleId)) continue;

        uint64_t sizeBytes = 0;
        sscanf(p2 + 1, "%" SCNu64, &sizeBytes);

        auto t = std::make_unique<TitleEntry>();
        t->titleId   = titleId;
        t->path      = p1 + 1;
        t->sizeBytes = sizeBytes;
        t->name      = p3 + 1;
        t->kind      = TitleId::IsVirtualConsole(titleId) ? TitleKind::GameVC : TitleKind::Game;
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

void TitleUninstaller::LoadTitleMetadata(TitleEntry& t) {
    std::string base = VolPathToFsPath(t.path);

    {
        std::string xmlPath = base + "/meta/meta.xml";
        FILE* f = fopen(xmlPath.c_str(), "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (t.sizeBytes == 0) {
                    const char* tag = strstr(line, "app_size");
                    if (tag) {
                        const char* s = strchr(tag, '>');
                        const char* e = s ? strchr(s + 1, '<') : nullptr;
                        if (s && e && e > s + 1) {
                            const char* vp = s + 1;
                            while (*vp == ' ' || *vp == '\t') vp++;
                            if (vp[0] == '0' && (vp[1] == 'x' || vp[1] == 'X')) vp += 2;
                            char* end = nullptr;
                            unsigned long long sz = strtoull(vp, &end, 16);
                            if (end != vp && sz > 0)
                                t.sizeBytes = (uint64_t)sz;
                        }
                    }
                }
                if (t.name.empty()) {
                    const char* tag = strstr(line, "shortname_en");
                    if (tag) {
                        const char* s = strchr(tag, '>');
                        const char* e = s ? strchr(s + 1, '<') : nullptr;
                        if (s && e && e > s + 1) {
                            std::string candidate(s + 1, e - s - 1);
                            while (!candidate.empty() &&
                                   (candidate.back() == '\n' || candidate.back() == '\r' || candidate.back() == ' '))
                                candidate.pop_back();
                            if (!candidate.empty()) t.name = candidate;
                        }
                    }
                }
                if (t.sizeBytes != 0 && !t.name.empty()) break;
            }
            fclose(f);
        }
    }

    ACPMetaXml* meta = static_cast<ACPMetaXml*>(memalign(0x40, sizeof(ACPMetaXml)));
    if (meta) {
        memset(meta, 0, sizeof(ACPMetaXml));
        if (ACPGetTitleMetaXml(t.titleId, meta) == 0) {
            if (meta->shortname_en[0] != '\0')
                t.name = meta->shortname_en;
            if (t.sizeBytes == 0 && meta->app_size > 0)
                t.sizeBytes = meta->app_size;
        }
        free(meta);
    }

    if (t.sizeBytes == 0) {
        t.sizeBytes = ReadTmdContentSize(base + "/code/title.tmd");
        if (t.sizeBytes == 0)
            t.sizeBytes = ReadTmdContentSize(base + "/meta/title.tmd");
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

    if (t.name.empty()) {
        char buf[17];
        snprintf(buf, sizeof(buf), "%016" PRIx64, t.titleId);
        t.name = buf;
    }

    t.iconPath   = base + "/meta/iconTex.tga";
    t.iconLoaded = false;
}

void TitleUninstaller::ScanComponents() {
    int32_t h = MCP_Open();
    if (h < 0) return;

    uint32_t total = MCP_TitleCount(h);
    if (total > 0) {
        std::vector<MCPTitleListType> list(total);
        uint32_t fetched = total;
        MCP_TitleList(h, &fetched, list.data(), fetched * sizeof(MCPTitleListType));
        for (uint32_t i = 0; i < fetched; i++) {
            const MCPTitleListType& info = list[i];
            bool onUSB  = TitleOnUSB(info);
            bool onNAND = TitleOnNAND(info);
            if (currentStorage == StorageLocation::USB  && !onUSB)  continue;
            if (currentStorage == StorageLocation::NAND && !onNAND) continue;

            uint64_t id = info.titleId;
            if (TitleId::IsUpdate(id) && !updateMap.count(TitleId::Low(id))) {
                auto t = std::make_unique<TitleEntry>();
                t->titleId = id;
                t->path    = info.path;
                t->kind    = TitleKind::Update;
                updateMap[TitleId::Low(id)] = std::move(t);
            } else if (TitleId::IsDLC(id) && !dlcMap.count(TitleId::Low(id))) {
                auto t = std::make_unique<TitleEntry>();
                t->titleId = id;
                t->path    = info.path;
                t->kind    = TitleKind::DLC;
                dlcMap[TitleId::Low(id)] = std::move(t);
            }
        }
    }
    MCP_Close(h);
}

void TitleUninstaller::LoadCheckedComponentMetadata() {
    ACPInitialize();
    for (const auto& cc : componentChoices) {
        uint32_t lowId = (uint32_t)(titles[cc.titleIdx]->titleId & 0xFFFFFFFF);
        auto u = updateMap.find(lowId);
        if (u != updateMap.end() && u->second->name.empty())
            LoadTitleMetadata(*u->second);
        auto d = dlcMap.find(lowId);
        if (d != dlcMap.end() && d->second->name.empty())
            LoadTitleMetadata(*d->second);
    }
    ACPFinalize();
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

    auto makeEntry = [](const MCPTitleListType& info, TitleKind kind) {
        auto t = std::make_unique<TitleEntry>();
        t->titleId = info.titleId;
        t->path    = info.path;
        t->kind    = kind;
        char buf[17];
        snprintf(buf, sizeof(buf), "%016" PRIx64, t->titleId);
        t->name = buf;
        return t;
    };

    if (count > 0) {
        std::vector<MCPTitleListType> list(count);
        uint32_t fetched  = count;
        uint32_t byteSize = count * sizeof(MCPTitleListType);
        MCP_TitleList(mcpHandle, &fetched, list.data(), byteSize);

        for (uint32_t i = 0; i < fetched; i++) {
            const MCPTitleListType& info = list[i];

            // Filter by active storage location
            bool onUSB  = TitleOnUSB(info);
            bool onNAND = TitleOnNAND(info);
            if (currentStorage == StorageLocation::USB  && !onUSB)  continue;
            if (currentStorage == StorageLocation::NAND && !onNAND) continue;

            uint64_t id = info.titleId;

            if (TitleId::IsBaseGame(id)) {
                bool isGame = (info.appType == MCP_APP_TYPE_GAME || info.appType == MCP_APP_TYPE_GAME_WII);
                if (!isGame) continue;

                auto t = makeEntry(info, TitleId::IsVirtualConsole(id) ? TitleKind::GameVC : TitleKind::Game);
                LoadTitleMetadata(*t);
                titles.push_back(std::move(t));

                if (titles.size() % 10 == 0) {
                    WHBProcIsRunning();
                }
            } else if (TitleId::IsUpdate(id)) {
                auto t = makeEntry(info, TitleKind::Update);
                LoadTitleMetadata(*t);
                updateMap[TitleId::Low(id)] = std::move(t);
            } else if (TitleId::IsDLC(id)) {
                auto t = makeEntry(info, TitleKind::DLC);
                LoadTitleMetadata(*t);
                dlcMap[TitleId::Low(id)] = std::move(t);
            }
        }
    }

    ApplySort();

    // Remove duplicate game names (regional variants share the same shortname_en)
    {
        std::set<std::string> seen;
        titles.erase(
            std::remove_if(titles.begin(), titles.end(),
                [&seen](const std::unique_ptr<TitleEntry>& t) {
                    if (seen.count(t->name)) return true;
                    seen.insert(t->name);
                    return false;
                }),
            titles.end());
    }

    // Close the scan handle, Start Uninstall opens a fresh one
    MCP_Close(mcpHandle);
    mcpHandle = -1;

    ACPFinalize();

    QueryStorage();

    LOG("Loaded %zu USB titles", titles.size());

    SaveTitleCache();
}

void TitleUninstaller::QueryStorage() {

    const char* mountPath = (currentStorage == StorageLocation::USB)
                            ? "/vol/storage_usb01" : "/vol/storage_mlc01";

    FSClient*   client = static_cast<FSClient*>(memalign(0x40, sizeof(FSClient)));
    FSCmdBlock* block  = static_cast<FSCmdBlock*>(memalign(0x40, sizeof(FSCmdBlock)));

    if (!client || !block) {
        if (client) free(client);
        if (block)  free(block);
        storageTotalBytes = 0;
        storageFreeBytes  = 0;
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
        storageFreeBytes  = freeSize;
        storageTotalBytes = (totalSize > 0) ? totalSize : freeSize;
        LOG("Storage (%s): total=%llu free=%llu", mountPath,
            (unsigned long long)storageTotalBytes,
            (unsigned long long)storageFreeBytes);
    } else {
        LOG("FSGetFreeSpaceSize failed for %s: %d", mountPath, (int)freeResult);
        storageTotalBytes = 0;
        storageFreeBytes  = 0;
    }
}
