#pragma once

#include "TitleDefs.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <coreinit/mcp.h>
#include <coreinit/energysaver.h>
#include <coreinit/systeminfo.h>

enum class TitleKind {
    Game,       // 0x00050000
    GameVC,     // 0x00050002
    Update,     // 0x0005000E
    DLC,        // 0x0005000C
};

struct TitleEntry {
    std::string  name;
    std::string  path;
    std::string  iconPath;
    SDL_Texture* icon;
    uint64_t     titleId;
    TitleKind    kind;
    uint64_t     sizeBytes;
    bool         checked;
    bool         iconLoaded;

    TitleEntry()
        : icon(nullptr), titleId(0), kind(TitleKind::Game),
          sizeBytes(0), checked(false), iconLoaded(false) {}
    ~TitleEntry();
};

struct ComponentChoice {
    int  titleIdx;
    bool wantGame;
    bool wantUpdate;
    bool wantDLC;
};

struct UninstallJob {
    std::string path;
    std::string name;
    int  removeIdx;
};

enum class AppState {
    Loading,
    List,
    ConfirmDelete,
    SelectComponents,
    Uninstalling,
    Done,
    Settings,
    InstallScan,
    InstallConfirm,
    Installing,
};

enum class AppMode {
    Uninstall,
    Install,
    COUNT,
};

class TitleUninstaller {
public:
    TitleUninstaller();
    ~TitleUninstaller();

    void Update(class Input& input);
    void Draw();

private:
    void LoadTitles();
    void LoadTitleMetadata(TitleEntry& t);
    void ScanComponents();
    void LoadCheckedComponentMetadata();
    void QueryStorage();
    void SaveTitleCache();
    bool LoadTitleCache();

    void SavePrefs();
    void LoadPrefs();

    void LoadNextPendingIcon();
    void LoadNextPendingIconFrom(std::vector<std::unique_ptr<TitleEntry>>& list);

    void StartUninstall();
    bool UninstallNext();

    void SwitchMode();
    void SwitchStorage();
    void EnterInstallMode();

    void ScanInstallSources();
    void StartInstall();
    bool InstallNext();

    void UpdateList(Input& input, float dt);
    void UpdateSelectComponents(Input& input);
    void UpdateConfirmDelete(Input& input);
    void UpdateUninstalling(Input&);
    void UpdateDone(Input& input);
    void UpdateSettings(Input& input);
    void UpdateInstallList(Input& input, float dt);
    void UpdateInstallConfirm(Input& input);
    void UpdateInstalling(Input&);

    void DrawBackground();
    void DrawTopBar();
    void DrawBottomBar();
    void DrawList();
    void DrawStoragePanel();
    void DrawConfirmDialog();
    void DrawComponentSelectDialog();
    void DrawUninstallProgress();
    void DrawDoneScreen();
    void DrawLoadingScreen();
    void DrawSettingsScreen();
    void DrawInstallList();
    void DrawTitleListImpl(const std::vector<std::unique_ptr<TitleEntry>>& list,
                           const char* emptyText, const char* emptySub);
    void DrawInstallConfirmDialog();
    void DrawInstallProgress();
    void DrawInstallLoadingScreen();
    void DrawInstallDoneScreen();

    std::string FormatSize(uint64_t bytes) const;
    int      CheckedCount() const;
    uint64_t CheckedBytes() const;
    int      InstallCheckedCount() const;
    uint64_t InstallCheckedBytes() const;
    void     ApplySort();
    void     ApplySortFor(std::vector<std::unique_ptr<TitleEntry>>& list);

    AppState state;
    AppMode  appMode;
    std::vector<std::unique_ptr<TitleEntry>> titles;
    std::vector<std::unique_ptr<TitleEntry>> installTitles;

    int  selectedIndex;
    bool loadingScreenShown;

    enum class SortMode { Alphabetical, SizeDesc, SizeAsc, COUNT };
    SortMode sortMode;

    enum class StorageLocation { USB, NAND };
    StorageLocation currentStorage;

    // Title lists per storage, both kept in memory so switching is instant
    std::vector<std::unique_ptr<TitleEntry>> titlesUSB;
    std::vector<std::unique_ptr<TitleEntry>> titlesNAND;

    std::map<uint32_t, std::unique_ptr<TitleEntry>> updateMap;
    std::map<uint32_t, std::unique_ptr<TitleEntry>> dlcMap;

    enum class ThemeMode { Dark, Light, COUNT };
    ThemeMode themeMode;
    int settingsSelectedItem;
    bool keepAwake;

    std::vector<UninstallJob> uninstallQueue;
    std::vector<UninstallJob> installQueue;
    std::vector<ComponentChoice> componentChoices;
    int componentFocusIdx;
    int componentFocusComponent;
    int  uninstallCurrent;
    int  uninstallSucceeded;
    int  uninstallFailed;
    bool uninstallInProgress;
    bool uninstallSeenActive;
    int  uninstallPollFrames;
    int32_t mcpHandle;
    MCPInstallTitleInfo mcpTitleInfo __attribute__((aligned(0x40)));

    int  installCurrent;
    int  installSucceeded;
    int  installFailed;
    bool installInProgress;
    bool installSeenActive;
    int  installPollFrames;
    uint64_t installSizeTotal;
    uint64_t installSizeProgress;
    uint32_t installContentsTotal;
    uint32_t installContentsProgress;
    bool lastOperationInstall;

    volatile bool    installMcpProcessing;
    volatile int32_t installMcpErr;
    static void InstallMcpCallback(MCPError err, void* rawData);

    uint64_t storageTotalBytes;
    uint64_t storageFreeBytes;

    uint32_t lastTick;
    float    selectionPulse;
    float    scrollAnim;
    int      targetScroll;
    float    highlightBarAnim;

    float    holdTimer;
    float    repeatAccum;

    static constexpr int LIST_X       = 60;
    static constexpr int LIST_Y       = 130;
    static constexpr int LIST_W       = 1300;
    static constexpr int ROW_H        = 110;
    static constexpr int VISIBLE_ROWS = 8;
    static constexpr int ICON_SIZE    = 80;
    static constexpr int PANEL_X      = 1390;
    static constexpr int PANEL_Y      = 130;
    static constexpr int PANEL_W      = 490;
};
