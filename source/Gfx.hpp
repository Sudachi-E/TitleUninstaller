#pragma once

#include <SDL.h>
#include <SDL_image.h>
#include <string>
#include <cmath>

namespace Gfx {
    constexpr uint32_t SCREEN_WIDTH  = 1920;
    constexpr uint32_t SCREEN_HEIGHT = 1080;

    struct Theme {
        SDL_Color BG_TOP;
        SDL_Color BG_BOTTOM;
        SDL_Color ACCENT;
        SDL_Color ACCENT_DARK;
        SDL_Color ACCENT_LIGHT;
        SDL_Color TEXT;
        SDL_Color TEXT_DIM;
        SDL_Color TEXT_SHADOW;
        SDL_Color SHADOW;
        SDL_Color OVERLAY;
        SDL_Color SELECTED;
        SDL_Color SIZE_TEXT;
        SDL_Color ROW_BG;
        SDL_Color ROW_BG_ALT;
        SDL_Color ROW_SELECTED;
        SDL_Color ROW_CHECKED;
        SDL_Color SEPARATOR;
        SDL_Color BAR_BG;       // top bar
        SDL_Color BAR_BOTTOM;   // bottom bar
        SDL_Color STORAGE_USED;
        SDL_Color STORAGE_FREE;
        SDL_Color DANGER;
        SDL_Color CHECK;
        SDL_Color PANEL_BG;
    };

    // Dark theme (default) — dark navy with amber accents
    inline Theme MakeDarkTheme() {
        return {
            /* BG_TOP        */ {0x16, 0x21, 0x3e, 0xff},
            /* BG_BOTTOM     */ {0x0f, 0x0f, 0x1a, 0xff},
            /* ACCENT        */ {0xf5, 0xb8, 0x00, 0xff},
            /* ACCENT_DARK   */ {0xd4, 0x9a, 0x00, 0xff},
            /* ACCENT_LIGHT  */ {0xff, 0xd0, 0x40, 0xff},
            /* TEXT          */ {0xff, 0xff, 0xff, 0xff},
            /* TEXT_DIM      */ {0x99, 0x99, 0xbb, 0xff},
            /* TEXT_SHADOW   */ {0x00, 0x00, 0x00, 0x80},
            /* SHADOW        */ {0x00, 0x00, 0x00, 0x40},
            /* OVERLAY       */ {0x00, 0x00, 0x00, 0xc0},
            /* SELECTED      */ {0xf5, 0xb8, 0x00, 0xff},
            /* SIZE_TEXT     */ {0xf5, 0xb8, 0x00, 0xff},
            /* ROW_BG        */ {0x1e, 0x28, 0x48, 0xff},
            /* ROW_BG_ALT    */ {0x1a, 0x22, 0x3e, 0xff},
            /* ROW_SELECTED  */ {0x2a, 0x30, 0x18, 0xff},
            /* ROW_CHECKED   */ {0x22, 0x2e, 0x14, 0xff},
            /* SEPARATOR     */ {0x2a, 0x35, 0x55, 0xff},
            /* BAR_BG        */ {0x0a, 0x0e, 0x20, 0xff},
            /* BAR_BOTTOM    */ {0x0a, 0x0e, 0x20, 0xff},
            /* STORAGE_USED  */ {0xf5, 0xb8, 0x00, 0xff},
            /* STORAGE_FREE  */ {0x2a, 0x35, 0x55, 0xff},
            /* DANGER        */ {0xe8, 0x3a, 0x2e, 0xff},
            /* CHECK         */ {0xf5, 0xb8, 0x00, 0xff},
            /* PANEL_BG      */ {0x1e, 0x28, 0x48, 0xff},
        };
    }

    // Light theme — warm off-white with amber accents
    inline Theme MakeLightTheme() {
        return {
            /* BG_TOP        */ {0xfa, 0xfa, 0xf5, 0xff},
            /* BG_BOTTOM     */ {0xec, 0xec, 0xe6, 0xff},
            /* ACCENT        */ {0xf5, 0xb8, 0x00, 0xff},
            /* ACCENT_DARK   */ {0xd4, 0x9a, 0x00, 0xff},
            /* ACCENT_LIGHT  */ {0xff, 0xd0, 0x40, 0xff},
            /* TEXT          */ {0x1a, 0x1a, 0x1a, 0xff},
            /* TEXT_DIM      */ {0x77, 0x77, 0x77, 0xff},
            /* TEXT_SHADOW   */ {0x00, 0x00, 0x00, 0x30},
            /* SHADOW        */ {0x00, 0x00, 0x00, 0x18},
            /* OVERLAY       */ {0x00, 0x00, 0x00, 0xb0},
            /* SELECTED      */ {0xf5, 0xb8, 0x00, 0xff},
            /* SIZE_TEXT     */ {0xd4, 0x9a, 0x00, 0xff},
            /* ROW_BG        */ {0xff, 0xff, 0xff, 0xff},
            /* ROW_BG_ALT    */ {0xfa, 0xfa, 0xf7, 0xff},
            /* ROW_SELECTED  */ {0xff, 0xf0, 0xcc, 0xff},
            /* ROW_CHECKED   */ {0xff, 0xf8, 0xe0, 0xff},
            /* SEPARATOR     */ {0xe0, 0xdc, 0xd0, 0xff},
            /* BAR_BG        */ {0xf5, 0xb8, 0x00, 0xff},
            /* BAR_BOTTOM    */ {0xff, 0xff, 0xff, 0xff},
            /* STORAGE_USED  */ {0xf5, 0xb8, 0x00, 0xff},
            /* STORAGE_FREE  */ {0xe8, 0xe4, 0xd8, 0xff},
            /* DANGER        */ {0xe8, 0x3a, 0x2e, 0xff},
            /* CHECK         */ {0xf5, 0xb8, 0x00, 0xff},
            /* PANEL_BG      */ {0xff, 0xff, 0xff, 0xff},
        };
    }

    extern Theme T;

    void SetTheme(const Theme& theme);

    // These are inline functions so they always read the current theme.
    inline SDL_Color COLOR_BG_TOP()       { return T.BG_TOP; }
    inline SDL_Color COLOR_BG_BOTTOM()    { return T.BG_BOTTOM; }
    inline SDL_Color COLOR_ACCENT()       { return T.ACCENT; }
    inline SDL_Color COLOR_ACCENT_DARK()  { return T.ACCENT_DARK; }
    inline SDL_Color COLOR_ACCENT_LIGHT() { return T.ACCENT_LIGHT; }
    inline SDL_Color COLOR_TEXT()         { return T.TEXT; }
    inline SDL_Color COLOR_TEXT_DIM()     { return T.TEXT_DIM; }
    inline SDL_Color COLOR_TEXT_SHADOW()  { return T.TEXT_SHADOW; }
    inline SDL_Color COLOR_SHADOW()       { return T.SHADOW; }
    inline SDL_Color COLOR_OVERLAY()      { return T.OVERLAY; }
    inline SDL_Color COLOR_SELECTED()     { return T.SELECTED; }
    inline SDL_Color COLOR_SIZE_TEXT()    { return T.SIZE_TEXT; }
    inline SDL_Color COLOR_ROW_BG()       { return T.ROW_BG; }
    inline SDL_Color COLOR_ROW_BG_ALT()   { return T.ROW_BG_ALT; }
    inline SDL_Color COLOR_ROW_SELECTED() { return T.ROW_SELECTED; }
    inline SDL_Color COLOR_ROW_CHECKED()  { return T.ROW_CHECKED; }
    inline SDL_Color COLOR_SEPARATOR()    { return T.SEPARATOR; }
    inline SDL_Color COLOR_BAR_BG()       { return T.BAR_BG; }
    inline SDL_Color COLOR_BAR_BOTTOM()   { return T.BAR_BOTTOM; }
    inline SDL_Color COLOR_STORAGE_USED() { return T.STORAGE_USED; }
    inline SDL_Color COLOR_STORAGE_FREE() { return T.STORAGE_FREE; }
    inline SDL_Color COLOR_DANGER()       { return T.DANGER; }
    inline SDL_Color COLOR_CHECK()        { return T.CHECK; }
    inline SDL_Color COLOR_PANEL_BG()     { return T.PANEL_BG; }

    constexpr SDL_Color COLOR_WHITE = {0xff, 0xff, 0xff, 0xff};
    constexpr SDL_Color COLOR_BLACK = {0x00, 0x00, 0x00, 0xff};

    enum AlignFlags {
        ALIGN_LEFT       = 1 << 0,
        ALIGN_RIGHT      = 1 << 1,
        ALIGN_HORIZONTAL = 1 << 2,
        ALIGN_TOP        = 1 << 3,
        ALIGN_BOTTOM     = 1 << 4,
        ALIGN_VERTICAL   = 1 << 5,
        ALIGN_CENTER     = ALIGN_HORIZONTAL | ALIGN_VERTICAL,
    };

    static constexpr inline AlignFlags operator|(AlignFlags lhs, AlignFlags rhs) {
        return static_cast<AlignFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    bool Init();
    void Shutdown();
    void Clear(SDL_Color color);
    void ClearGradient(SDL_Color topColor, SDL_Color bottomColor);
    void Render();
    void DrawRectFilled(int x, int y, int w, int h, SDL_Color color);
    void DrawRectOutline(int x, int y, int w, int h, SDL_Color color, int thickness = 2);
    void DrawRectRounded(int x, int y, int w, int h, int radius, SDL_Color color);
    void DrawRectRoundedOutline(int x, int y, int w, int h, int radius, SDL_Color color, int thickness = 3);
    void DrawRectGradient(int x, int y, int w, int h, SDL_Color topColor, SDL_Color bottomColor);
    void DrawCircleFilled(int cx, int cy, int radius, SDL_Color color);
    void Print(int x, int y, int size, SDL_Color color, const std::string& text, AlignFlags align = ALIGN_LEFT | ALIGN_TOP);
    void PrintWithShadow(int x, int y, int size, SDL_Color color, const std::string& text, AlignFlags align = ALIGN_LEFT | ALIGN_TOP);
    void PrintIcon(int x, int y, int size, SDL_Color color, const std::string& text, AlignFlags align = ALIGN_LEFT | ALIGN_TOP);
    int GetIconTextWidth(int size, const std::string& text);
    int GetTextWidth(int size, const std::string& text);
    int GetTextHeight(int size, const std::string& text);
    SDL_Texture* LoadTexture(const std::string& path);
    void DrawTexture(SDL_Texture* texture, int x, int y, int w, int h, uint8_t alpha = 255);
    SDL_Renderer* GetRenderer();
    uint32_t GetTicks();
}
