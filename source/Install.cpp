#include "TitleUninstaller.hpp"
#include "TitleDefs.hpp"
#include <coreinit/mcp.h>
#include <dirent.h>
#include <sys/stat.h>
#include <whb/proc.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool ReadWupInfo(const std::string& tmdPath, uint64_t& titleId,
                 uint64_t& sizeBytes, TitleKind& kind) {
    FILE* f = fopen(tmdPath.c_str(), "rb");
    if (!f) return false;

    long fileSize = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    if (fileSize < 0x1E4 + 0x30) { fclose(f); return false; }

    std::vector<uint8_t> hdr(0x1E4);
    if (fread(hdr.data(), 1, hdr.size(), f) != hdr.size()) { fclose(f); return false; }

    uint8_t  version     = hdr[0x180];
    uint64_t tid         = 0;
    memcpy(&tid, &hdr[0x18C], sizeof(tid));
    uint16_t numContents = 0;
    memcpy(&numContents, &hdr[0x1DE], sizeof(numContents));

    uint32_t contentsOff = (version == 1) ? 0x0B04 : 0x1E4;
    size_t   recBytes    = (size_t)numContents * 0x30;
    uint64_t total       = 0;

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
    fclose(f);

    if (tid == 0) return false;

    titleId   = tid;
    sizeBytes = total;
    uint64_t high = TitleId::High(tid);
    if (high == TitleId::UPDATE)       kind = TitleKind::Update;
    else if (high == TitleId::DLC)     kind = TitleKind::DLC;
    else if (high == TitleId::GAME_VC) kind = TitleKind::GameVC;
    else                               kind = TitleKind::Game;
    return true;
}

std::string ReadShortname(const std::string& xmlPath) {
    FILE* f = fopen(xmlPath.c_str(), "r");
    if (!f) return "";

    std::string name;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        const char* tag = strstr(line, "shortname_en");
        if (!tag) continue;
        const char* s = strchr(tag, '>');
        const char* e = s ? strchr(s + 1, '<') : nullptr;
        if (s && e && e > s + 1) {
            std::string candidate(s + 1, e - s - 1);
            while (!candidate.empty() &&
                   (candidate.back() == '\n' || candidate.back() == '\r' || candidate.back() == ' '))
                candidate.pop_back();
            if (!candidate.empty()) name = candidate;
        }
        break;
    }
    fclose(f);
    return name;
}

void ScanDir(const std::string& dir, int depth,
             std::vector<std::unique_ptr<TitleEntry>>& out) {
    if (depth > 6) return;

    DIR* d = opendir(dir.c_str());
    if (!d) return;

    std::vector<std::string> subdirs;
    bool hasTmd = false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string fp = dir + "/" + e->d_name;
        struct stat st;
        if (stat(fp.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            subdirs.push_back(fp);
        } else if (strcmp(e->d_name, "title.tmd") == 0) {
            hasTmd = true;
        }
    }
    closedir(d);

    if (hasTmd) {
        uint64_t tid = 0, size = 0;
        TitleKind kind = TitleKind::Game;
        if (ReadWupInfo(dir + "/title.tmd", tid, size, kind)) {
            auto t = std::make_unique<TitleEntry>();
            t->titleId   = tid;
            if (dir.compare(0, 3, "fs:") == 0) {
                const std::string prefix = "fs:/vol/external01";
                if (dir.compare(0, prefix.size(), prefix) == 0)
                    t->path = "/vol/app_sd" + dir.substr(prefix.size());
                else
                    t->path = dir.substr(3);
            } else {
                t->path = dir;
            }
            t->sizeBytes = size;
            t->kind      = kind;

            t->name = ReadShortname(dir + "/meta/meta.xml");
            if (t->name.empty()) t->name = ReadShortname(dir + "/meta.xml");
            if (t->name.empty()) {
                size_t slash = dir.find_last_of('/');
                t->name = (slash == std::string::npos) ? dir : dir.substr(slash + 1);
            }
            if (t->name.empty()) {
                char buf[17];
                snprintf(buf, sizeof(buf), "%016" PRIx64, tid);
                t->name = buf;
            }

            std::string icon = dir + "/meta/iconTex.tga";
            struct stat st;
            if (stat(icon.c_str(), &st) == 0)
                t->iconPath = icon;
            t->iconLoaded = false;

            out.push_back(std::move(t));
            if (out.size() % 10 == 0) WHBProcIsRunning();
        }
        return;
    }

    for (const auto& sd : subdirs)
        ScanDir(sd, depth + 1, out);
}

}

void TitleUninstaller::InstallMcpCallback(MCPError err, void* rawData) {
    TitleUninstaller* self = static_cast<TitleUninstaller*>(rawData);
    if (self->installMcpErr == 0)
        self->installMcpErr = err;
    self->installMcpProcessing = false;
}

void TitleUninstaller::ScanInstallSources() {
    installTitles.clear();
    ScanDir("fs:/vol/external01/install", 0, installTitles);
    ApplySortFor(installTitles);
    LOG("ScanInstallSources: %zu installable title(s) found", installTitles.size());
}

void TitleUninstaller::StartInstall() {
    installQueue.clear();
    for (int i = 0; i < (int)installTitles.size(); i++) {
        if (installTitles[i]->checked) {
            UninstallJob job;
            job.path = installTitles[i]->path;
            job.name = installTitles[i]->name;
            job.removeIdx = -1;
            installQueue.push_back(job);
        }
    }

    installCurrent           = 0;
    installSucceeded         = 0;
    installFailed            = 0;
    installInProgress        = false;
    installSeenActive        = false;
    installPollFrames        = 0;
    installSizeTotal         = 0;
    installSizeProgress      = 0;
    installContentsTotal     = 0;
    installContentsProgress  = 0;
    installMcpProcessing     = true;
    installMcpErr            = 0;

    if (mcpHandle >= 0) { MCP_Close(mcpHandle); mcpHandle = -1; }
    mcpHandle = MCP_Open();
    if (mcpHandle < 0) {
        LOG("StartInstall: MCP_Open failed");
        return;
    }

    OSEnableHomeButtonMenu(FALSE);
    LOG("Home button menu disabled during install");

    lastOperationInstall = true;
    state = AppState::Installing;
    LOG("Starting batch install of %zu jobs", installQueue.size());
}

bool TitleUninstaller::InstallNext() {
    if (installCurrent >= (int)installQueue.size()) {
        if (mcpHandle >= 0) { MCP_Close(mcpHandle); mcpHandle = -1; }

        OSEnableHomeButtonMenu(TRUE);
        LOG("Home button menu re-enabled");

        state = AppState::Done;
        return false;
    }

    UninstallJob& job = installQueue[installCurrent];

    if (!installInProgress) {
        LOG("Installing: %s  path=%s", job.name.c_str(), job.path.c_str());
        memset(&mcpTitleInfo, 0, sizeof(mcpTitleInfo));

        MCPError err = MCP_InstallGetInfo(mcpHandle, job.path.c_str(),
                                          (MCPInstallInfo*)&mcpTitleInfo);
        if (err != 0) {
            LOG("MCP_InstallGetInfo failed for %s: %#010x", job.name.c_str(), (int)err);
            installFailed++;
            installCurrent++;
            return true;
        }

        MCPInstallTarget target = (currentStorage == StorageLocation::USB)
                                  ? MCP_INSTALL_TARGET_USB : MCP_INSTALL_TARGET_MLC;
        err = MCP_InstallSetTargetDevice(mcpHandle, target);
        if (err != 0) {
            LOG("MCP_InstallSetTargetDevice failed for %s: %#010x", job.name.c_str(), (int)err);
            installFailed++;
            installCurrent++;
            return true;
        }

        installMcpProcessing = true;
        installMcpErr        = 0;
        uint32_t* mcpHdr = (uint32_t*)&mcpTitleInfo;
        mcpHdr[0] = (uint32_t)InstallMcpCallback;
        mcpHdr[1] = (uint32_t)(uintptr_t)this;

        err = MCP_InstallTitleAsync(mcpHandle, job.path.c_str(), &mcpTitleInfo);
        if (err != 0) {
            LOG("MCP_InstallTitleAsync failed for %s: %#010x", job.name.c_str(), (int)err);
            installFailed++;
            installCurrent++;
            return true;
        }

        installInProgress       = true;
        installSeenActive       = false;
        installPollFrames       = 0;
        installSizeTotal        = 0;
        installSizeProgress     = 0;
        installContentsTotal    = 0;
        installContentsProgress = 0;
        return true;
    }

    MCPInstallProgress prog __attribute__((aligned(0x40)));
    memset(&prog, 0, sizeof(prog));
    MCP_InstallGetProgress(mcpHandle, &prog);
    installPollFrames++;

    installSizeTotal        = prog.sizeTotal;
    installSizeProgress     = prog.sizeProgress;
    installContentsTotal    = prog.contentsTotal;
    installContentsProgress = prog.contentsProgress;

    LOG("Poll[%d] '%s': inProgress=%u size=%llu/%llu",
        installPollFrames, job.name.c_str(), prog.inProgress,
        (unsigned long long)prog.sizeProgress,
        (unsigned long long)prog.sizeTotal);

    if (prog.inProgress == 1) {
        installSeenActive = true;
    }

    bool done = !installMcpProcessing;
    if (!done && installSeenActive && prog.inProgress == 0 && installPollFrames > 10)
        done = true;

    if (done) {
        int32_t err = installMcpErr;
        if (installMcpProcessing) err = 0;

        if (err == 0) {
            LOG("Install complete for '%s' (frames=%d)", job.name.c_str(), installPollFrames);
            installSucceeded++;
        } else {
            LOG("Install failed for '%s': %#010x", job.name.c_str(), (int)err);
            installFailed++;
        }

        installInProgress  = false;
        installSeenActive  = false;
        installPollFrames  = 0;
        installCurrent++;

        if (installCurrent < (int)installQueue.size()) {
            MCP_Close(mcpHandle);
            mcpHandle = MCP_Open();
            if (mcpHandle < 0) {
                LOG("MCP_Open failed between installs — aborting");
                installFailed += (int)installQueue.size() - installCurrent;
                installCurrent = (int)installQueue.size();
            }
        }
    }

    return true;
}
