#include "TitleUninstaller.hpp"
#include "TitleDefs.hpp"
#include "Gfx.hpp"
#include "Input.hpp"
#include <SDL.h>
#include <coreinit/mcp.h>
#include <algorithm>
#include <cstdio>

TitleEntry::~TitleEntry() {
    if (icon) {
        SDL_DestroyTexture(icon);
        icon = nullptr;
    }
}

TitleUninstaller::TitleUninstaller()
    : state(AppState::Loading),
      selectedIndex(0), loadingScreenShown(false),
      sortMode(SortMode::Alphabetical),
      currentStorage(StorageLocation::USB),
      themeMode(ThemeMode::Dark), settingsSelectedItem(0), keepAwake(false),
      uninstallCurrent(0), uninstallSucceeded(0), uninstallFailed(0),
      uninstallInProgress(false), uninstallSeenActive(false), uninstallPollFrames(0), mcpHandle(-1),
      storageTotalBytes(0), storageFreeBytes(0),
      lastTick(0), selectionPulse(0.0f), scrollAnim(0.0f), targetScroll(0),
      highlightBarAnim(0.0f), holdTimer(0.0f), repeatAccum(0.0f)
{
    memset(&mcpTitleInfo, 0, sizeof(mcpTitleInfo));

    Gfx::SetTheme(Gfx::MakeDarkTheme());
    LoadPrefs();

    if (LoadTitleCache()) {
        LOG("Cache hit: %zu titles, skipping scan", titles.size());

        auto& primaryList = (currentStorage == StorageLocation::USB)
                            ? titlesUSB : titlesNAND;
        primaryList = std::move(titles);

        StorageLocation other = (currentStorage == StorageLocation::USB)
                                ? StorageLocation::NAND : StorageLocation::USB;
        currentStorage = other;
        if (LoadTitleCache()) {
            auto& secondaryList = (other == StorageLocation::USB)
                                  ? titlesUSB : titlesNAND;
            secondaryList = std::move(titles);
        }

        currentStorage = (other == StorageLocation::USB)
                         ? StorageLocation::NAND : StorageLocation::USB;
        titles = std::move(primaryList);

        QueryStorage();
        state = AppState::List;
    }
}

TitleUninstaller::~TitleUninstaller() {
    OSReport("[EXIT] ~TitleUninstaller begin\n");
    if (keepAwake) {
        IMEnableAPD();
        IMEnableDim();
    }
    OSReport("[EXIT] Closing MCP handle=%d\n", mcpHandle);
    if (mcpHandle >= 0) { MCP_Close(mcpHandle); mcpHandle = -1; }
    OSReport("[EXIT] MCP_Close done\n");
    OSReport("[EXIT] Clearing %zu title entries\n", titles.size());
    titles.clear();
    titlesUSB.clear();
    titlesNAND.clear();
    OSReport("[EXIT] ~TitleUninstaller complete\n");
}

void TitleUninstaller::SavePrefs() {
    EnsureCacheDir();
    FILE* f = fopen(CachePath::Prefs(), "w");
    if (!f) return;
    fprintf(f, "theme=%d\n", (int)themeMode);
    fprintf(f, "storage=%d\n", (int)currentStorage);
    fprintf(f, "keepAwake=%d\n", keepAwake ? 1 : 0);
    fclose(f);
}

void TitleUninstaller::LoadPrefs() {
    FILE* f = fopen(CachePath::Prefs(), "r");
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
        if (sscanf(line, "storage=%d", &val) == 1) {
            currentStorage = (val == (int)StorageLocation::NAND)
                             ? StorageLocation::NAND : StorageLocation::USB;
        }
        if (sscanf(line, "keepAwake=%d", &val) == 1) {
            keepAwake = (val == 1);
            if (keepAwake) {
                IMDisableAPD();
                IMDisableDim();
            }
        }
    }
    fclose(f);
}

void TitleUninstaller::Update(Input& input) {
    uint32_t now = Gfx::GetTicks();
    float dt = (lastTick > 0) ? (now - lastTick) / 1000.0f : 0.016f;
    lastTick = now;
    selectionPulse += dt * 3.0f;

    float diff = (float)targetScroll - scrollAnim;
    scrollAnim += diff * 0.18f;

    float targetFrac = 0.0f;
    if (storageTotalBytes > 0 && !titles.empty() &&
        selectedIndex >= 0 && selectedIndex < (int)titles.size()) {
        targetFrac = (float)titles[selectedIndex]->sizeBytes / (float)storageTotalBytes;
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

    case AppState::List:
        UpdateList(input, dt);
        break;

    case AppState::SelectComponents:
        UpdateSelectComponents(input);
        break;

    case AppState::ConfirmDelete:
        UpdateConfirmDelete(input);
        break;

    case AppState::Uninstalling:
        UpdateUninstalling(input);
        break;

    case AppState::Done:
        UpdateDone(input);
        break;

    case AppState::Settings:
        UpdateSettings(input);
        break;
    }
}

void TitleUninstaller::UpdateList(Input& input, float dt) {
    LoadNextPendingIcon();

    int maxIdx = (int)titles.size() - 1;

    bool holdingDown = input.IsHeld(Input::BUTTON_DOWN);
    bool holdingUp   = input.IsHeld(Input::BUTTON_UP);
    bool holding     = holdingDown || holdingUp;

    int navDelta = 0;

    if (input.IsPressed(Input::BUTTON_DOWN) || input.IsPressed(Input::BUTTON_UP)) {
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

        int steps = std::min(std::abs(navDelta), (int)titles.size());
        int dir   = (navDelta > 0) ? 1 : -1;
        for (int s = 0; s < steps; s++) moveSelection(dir);
    }

    if (input.IsPressed(Input::BUTTON_A)) {
        if (!titles.empty())
            titles[selectedIndex]->checked = !titles[selectedIndex]->checked;
    }

    if (input.IsPressed(Input::BUTTON_Y)) {
        bool anyUnchecked = false;
        for (const auto& t : titles) if (!t->checked) { anyUnchecked = true; break; }
        for (auto& t : titles) t->checked = anyUnchecked;
    }

    // Delete flow (with update/DLC component selection when applicable)
    if (input.IsPressed(Input::BUTTON_PLUS)) {
        if (CheckedCount() > 0) {
            if (updateMap.empty() && dlcMap.empty()) {
                ScanComponents();
            }
            bool hasComponents = false;
            for (const auto& t : titles) {
                if (t->checked) {
                    uint32_t lowId = (uint32_t)(t->titleId & 0xFFFFFFFF);
                    if (updateMap.count(lowId) || dlcMap.count(lowId)) {
                        hasComponents = true;
                        break;
                    }
                }
            }
            if (hasComponents) {
                componentChoices.clear();
                for (int i = 0; i < (int)titles.size(); i++) {
                    if (titles[i]->checked) {
                        ComponentChoice cc;
                        cc.titleIdx  = i;
                        cc.wantGame  = true;
                        cc.wantUpdate = false;
                        cc.wantDLC    = false;
                        componentChoices.push_back(cc);
                    }
                }
                componentFocusIdx = 0;
                componentFocusComponent = 0;
                LoadCheckedComponentMetadata();
                state = AppState::SelectComponents;
            } else {
                state = AppState::ConfirmDelete;
            }
        }
    }

    if (input.IsPressed(Input::BUTTON_X)) {
        const char* path = (currentStorage == StorageLocation::USB)
                           ? CachePath::USB() : CachePath::NAND();
        remove(path);
        titles.clear();
        selectedIndex      = 0;
        targetScroll       = 0;
        scrollAnim         = 0.0f;
        loadingScreenShown = false;
        state = AppState::Loading;
    }

    if (input.IsPressed(Input::BUTTON_MINUS)) {
        settingsSelectedItem = 0;
        state = AppState::Settings;
    }

    if (input.IsPressed(Input::BUTTON_L) || input.IsPressed(Input::BUTTON_R)) {
        int next = (int)sortMode;
        if (input.IsPressed(Input::BUTTON_R))
            next = (next + 1) % (int)SortMode::COUNT;
        else
            next = (next - 1 + (int)SortMode::COUNT) % (int)SortMode::COUNT;
        sortMode = (SortMode)next;
        ApplySort();
        selectedIndex = 0;
        targetScroll  = 0;
        scrollAnim    = 0.0f;
    }

    if (input.IsPressed(Input::BUTTON_ZL) || input.IsPressed(Input::BUTTON_ZR)) {
        SwitchStorage();
    }
}

void TitleUninstaller::SwitchStorage() {
    updateMap.clear();
    dlcMap.clear();

    if (currentStorage == StorageLocation::USB)
        titlesUSB  = std::move(titles);
    else
        titlesNAND = std::move(titles);

    currentStorage = (currentStorage == StorageLocation::USB)
                     ? StorageLocation::NAND : StorageLocation::USB;
    SavePrefs();
    auto& otherList = (currentStorage == StorageLocation::USB)
                      ? titlesUSB : titlesNAND;

    if (!otherList.empty()) {
        titles = std::move(otherList);
        selectedIndex = 0;
        targetScroll  = 0;
        scrollAnim    = 0.0f;
        storageTotalBytes = 0;
        storageFreeBytes  = 0;
        QueryStorage();
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
        storageTotalBytes  = 0;
        storageFreeBytes   = 0;
        state = AppState::Loading;
    }
}

void TitleUninstaller::UpdateSelectComponents(Input& input) {
    if (componentChoices.empty()) {
        state = AppState::List;
        return;
    }
    const int rows = (int)componentChoices.size();

    if (input.IsPressed(Input::BUTTON_UP)) {
        componentFocusIdx = (componentFocusIdx - 1 + rows) % rows;
    }
    if (input.IsPressed(Input::BUTTON_DOWN)) {
        componentFocusIdx = (componentFocusIdx + 1) % rows;
    }
    if (input.IsPressed(Input::BUTTON_LEFT)) {
        componentFocusComponent = (componentFocusComponent - 1 + 3) % 3;
    }
    if (input.IsPressed(Input::BUTTON_RIGHT)) {
        componentFocusComponent = (componentFocusComponent + 1) % 3;
    }
    if (input.IsPressed(Input::BUTTON_A)) {
        ComponentChoice& cc = componentChoices[componentFocusIdx];
        uint32_t lowId = (uint32_t)(titles[cc.titleIdx]->titleId & 0xFFFFFFFF);
        bool hasUpdate = updateMap.count(lowId) > 0;
        bool hasDLC    = dlcMap.count(lowId) > 0;
        switch (componentFocusComponent) {
        case 0: cc.wantGame = !cc.wantGame; break;
        case 1: if (hasUpdate) cc.wantUpdate = !cc.wantUpdate; break;
        case 2: if (hasDLC)    cc.wantDLC    = !cc.wantDLC;    break;
        }
    }
    if (input.IsPressed(Input::BUTTON_PLUS)) {
        state = AppState::ConfirmDelete;
    }
    if (input.IsPressed(Input::BUTTON_B)) {
        componentChoices.clear();
        state = AppState::List;
    }
}

void TitleUninstaller::UpdateConfirmDelete(Input& input) {
    if (input.IsPressed(Input::BUTTON_A)) {
        StartUninstall();
    }
    if (input.IsPressed(Input::BUTTON_B)) {
        state = AppState::List;
    }
}

void TitleUninstaller::UpdateUninstalling(Input&) {
    UninstallNext();
}

void TitleUninstaller::UpdateDone(Input& input) {
    if (input.IsPressed(Input::BUTTON_A)) {
        for (auto& t : titles) t->checked = false;
        selectedIndex      = std::min(selectedIndex, std::max(0, (int)titles.size() - 1));
        targetScroll       = std::min(targetScroll,  std::max(0, (int)titles.size() - 1));
        scrollAnim         = (float)targetScroll;
        state = AppState::List;
    }
}

void TitleUninstaller::UpdateSettings(Input& input) {
    if (input.IsPressed(Input::BUTTON_UP))
        settingsSelectedItem = std::max(0, settingsSelectedItem - 1);
    if (input.IsPressed(Input::BUTTON_DOWN))
        settingsSelectedItem = std::min(1, settingsSelectedItem + 1);

    if (input.IsPressed(Input::BUTTON_A)) {
        if (settingsSelectedItem == 0) {
            int next = ((int)themeMode + 1) % (int)ThemeMode::COUNT;
            themeMode = (ThemeMode)next;
            if (themeMode == ThemeMode::Dark)
                Gfx::SetTheme(Gfx::MakeDarkTheme());
            else
                Gfx::SetTheme(Gfx::MakeLightTheme());
            SavePrefs();
        } else if (settingsSelectedItem == 1) {
            keepAwake = !keepAwake;
            if (keepAwake) {
                IMDisableAPD();
                IMDisableDim();
            } else {
                IMEnableAPD();
                IMEnableDim();
            }
            SavePrefs();
        }
    }

    if (input.IsPressed(Input::BUTTON_B) || input.IsPressed(Input::BUTTON_MINUS)) {
        state = AppState::List;
    }
}

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

    case AppState::SelectComponents:
        DrawBackground();
        DrawTopBar();
        DrawList();
        DrawStoragePanel();
        DrawBottomBar();
        DrawComponentSelectDialog();
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
