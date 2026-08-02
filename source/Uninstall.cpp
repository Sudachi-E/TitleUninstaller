#include "TitleUninstaller.hpp"
#include "TitleDefs.hpp"
#include <coreinit/mcp.h>
#include <algorithm>
#include <set>

void TitleUninstaller::StartUninstall() {
    uninstallQueue.clear();

    if (!componentChoices.empty()) {
        // Queue built from the component-selection dialog: each checked game
        // may also carry its update and/or DLC title.
        for (const auto& cc : componentChoices) {
            TitleEntry& t = *titles[cc.titleIdx];
            uint32_t lowId = (uint32_t)(t.titleId & 0xFFFFFFFF);
            if (cc.wantGame) {
                UninstallJob job;
                job.path = t.path;
                job.name = t.name;
                job.removeIdx = cc.titleIdx;
                uninstallQueue.push_back(job);
            }
            if (cc.wantUpdate) {
                auto it = updateMap.find(lowId);
                if (it != updateMap.end()) {
                    UninstallJob job;
                    job.path = it->second->path;
                    job.name = it->second->name + " (Update)";
                    job.removeIdx = -1;
                    uninstallQueue.push_back(job);
                }
            }
            if (cc.wantDLC) {
                auto it = dlcMap.find(lowId);
                if (it != dlcMap.end()) {
                    UninstallJob job;
                    job.path = it->second->path;
                    job.name = it->second->name + " (DLC)";
                    job.removeIdx = -1;
                    uninstallQueue.push_back(job);
                }
            }
            if (!cc.wantGame) {
                titles[cc.titleIdx]->checked = false;
            }
        }
        componentChoices.clear();
    } else {
        // Simple path: every checked title in the list
        for (int i = 0; i < (int)titles.size(); i++) {
            if (titles[i]->checked) {
                UninstallJob job;
                job.path = titles[i]->path;
                job.name = titles[i]->name;
                job.removeIdx = i;
                uninstallQueue.push_back(job);
            }
        }
    }

    uninstallCurrent    = 0;
    uninstallSucceeded  = 0;
    uninstallFailed     = 0;
    uninstallInProgress = false;
    uninstallSeenActive = false;
    uninstallPollFrames = 0;

    if (mcpHandle >= 0) { MCP_Close(mcpHandle); mcpHandle = -1; }
    mcpHandle = MCP_Open();
    if (mcpHandle < 0) {
        LOG("StartUninstall: MCP_Open failed");
        return;
    }

    OSEnableHomeButtonMenu(FALSE);
    LOG("Home button menu disabled during uninstall");

    lastOperationInstall = false;
    state = AppState::Uninstalling;
    LOG("Starting batch uninstall of %zu jobs", uninstallQueue.size());
}

bool TitleUninstaller::UninstallNext() {
    if (uninstallCurrent >= (int)uninstallQueue.size()) {
        if (mcpHandle >= 0) { MCP_Close(mcpHandle); mcpHandle = -1; }

        std::set<int> removeSet;
        for (const auto& job : uninstallQueue) {
            if (job.removeIdx >= 0) removeSet.insert(job.removeIdx);
        }
        if (!removeSet.empty()) {
            std::vector<std::unique_ptr<TitleEntry>> remaining;
            for (int i = 0; i < (int)titles.size(); i++) {
                if (removeSet.count(i)) continue;
                remaining.push_back(std::move(titles[i]));
            }
            titles = std::move(remaining);
        }

        if (selectedIndex >= (int)titles.size())
            selectedIndex = std::max(0, (int)titles.size() - 1);
        if (targetScroll >= (int)titles.size())
            targetScroll = std::max(0, (int)titles.size() - 1);

        SaveTitleCache();

        // Refresh update/DLC maps so stale entries don't persist
        updateMap.clear();
        dlcMap.clear();
        ScanComponents();

        OSEnableHomeButtonMenu(TRUE);
        LOG("Home button menu re-enabled");

        state = AppState::Done;
        return false;
    }

    UninstallJob& job = uninstallQueue[uninstallCurrent];

    if (!uninstallInProgress) {
        LOG("Uninstalling: %s  path=%s", job.name.c_str(), job.path.c_str());
        memset(&mcpTitleInfo, 0, sizeof(mcpTitleInfo));
        MCPError err = MCP_UninstallTitleAsync(mcpHandle, job.path.c_str(), &mcpTitleInfo);
        if (err != 0) {
            LOG("MCP_UninstallTitleAsync failed for %s: %d", job.name.c_str(), (int)err);
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
        uninstallPollFrames, job.name.c_str(), prog.inProgress, prog.contentsTotal);

    if (prog.inProgress == 1) {
        uninstallSeenActive = true;
    } else if (prog.inProgress == 0) {
        if (uninstallSeenActive || uninstallPollFrames > 10) {
            LOG("Uninstall complete for '%s' (frames=%d seenActive=%d)",
                job.name.c_str(), uninstallPollFrames, (int)uninstallSeenActive);
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
