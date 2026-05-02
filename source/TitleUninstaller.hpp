#pragma once

#include <SDL.h>
#include <SDL_image.h>
#include <string>
#include <vector>
#include <memory>
#include <coreinit/mcp.h>

// Title entry

enum class TitleKind {
    Game,   // 0x00050000
    Update, // 0x0005000E
    DLC,    // 0x0005000C
};

struct TitleEntry {
    std::string  name;
    std::string  path;       // /vol/ path from MCP
    std::string  iconPath;   // fs:/ path for icon loading
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

// state machine

enum class AppState {
    Loading,
    List,
    ConfirmDelete,
    Uninstalling,
    Done,
    Settings,
};

// Main class

class TitleUninstaller {
public:
    TitleUninstaller();
    ~TitleUninstaller();

    void Update(class Input& input);
    void Draw();

    void StopIconThread();

private:
    void LoadTitles();
    void LoadTitleMetadata(TitleEntry& t);
    void QueryUSBStorage();
    void QueryStorage();
    void SaveTitleCache();
    bool LoadTitleCache();
    void SavePrefs();
    void LoadPrefs();

    void LoadNextPendingIcon();

    void StartUninstall();
    bool UninstallNext();

    void DrawBackground();
    void DrawTopBar();
    void DrawBottomBar();
    void DrawList();
    void DrawStoragePanel();
    void DrawConfirmDialog();
    void DrawUninstallProgress();
    void DrawDoneScreen();
    void DrawLoadingScreen();
    void DrawSettingsScreen();

    std::string FormatSize(uint64_t bytes) const;
    int      CheckedCount() const;
    uint64_t CheckedBytes() const;
    void     ApplySort();

    AppState state;
    std::vector<std::unique_ptr<TitleEntry>> titles;

    int  selectedIndex;
    bool loadingScreenShown;

    enum class SortMode { Alphabetical, SizeDesc, SizeAsc, COUNT };
    SortMode sortMode;

    enum class StorageLocation { USB, NAND };
    StorageLocation currentStorage;

    // Title lists per storage — both kept in memory so switching is instant
    std::vector<std::unique_ptr<TitleEntry>> titlesUSB;
    std::vector<std::unique_ptr<TitleEntry>> titlesNAND;

    // Theme
    enum class ThemeMode { Dark, Light, COUNT };
    ThemeMode themeMode;
    int settingsSelectedItem;

    // Uninstall
    std::vector<int> uninstallQueue;
    int  uninstallCurrent;
    int  uninstallSucceeded;
    int  uninstallFailed;
    bool uninstallInProgress;
    bool uninstallSeenActive;
    int  uninstallPollFrames;
    int32_t mcpHandle;
    MCPInstallTitleInfo mcpTitleInfo;

    // Storage info
    uint64_t usbTotalBytes;
    uint64_t usbFreeBytes;

    // Animation
    uint32_t lastTick;
    float    selectionPulse;
    float    scrollAnim;
    int      targetScroll;
    float    highlightBarAnim; // smoothly animated width of the green bar (0-1 fraction of barWidth)

    // D-pad hold-repeat state
    float    holdTimer;       // seconds the current direction has been held
    float    repeatAccum;     // accumulator for repeat firing

    // Layout constants
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
