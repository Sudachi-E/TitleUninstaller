#include "Gfx.hpp"
#include <SDL_ttf.h>
#include <coreinit/memory.h>
#include <coreinit/debug.h>
#include "DejaVuSans_ttf.h"
#include <map>

namespace Gfx {
    //  The Active theme — dark theme by default
    Theme T = MakeDarkTheme();

    void SetTheme(const Theme& theme) { T = theme; }
    static SDL_Window*   sWindow   = nullptr;
    static SDL_Renderer* sRenderer = nullptr;
    static TTF_Font*     sFont     = nullptr;
    static TTF_Font*     sIconFont = nullptr;
    static std::map<int, TTF_Font*> sIconFontBySize;
    static void*    sFontData = nullptr;
    static uint32_t sFontSize = 0;

    bool Init() {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) return false;

        if (TTF_Init() < 0) {
            SDL_Quit();
            return false;
        }

        int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
        if (!(IMG_Init(imgFlags) & imgFlags)) {
            TTF_Quit();
            SDL_Quit();
            return false;
        }

        sWindow = SDL_CreateWindow("WiiU Title Uninstaller", 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0);
        if (!sWindow) { IMG_Quit(); TTF_Quit(); SDL_Quit(); return false; }

        sRenderer = SDL_CreateRenderer(sWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!sRenderer) { SDL_DestroyWindow(sWindow); IMG_Quit(); TTF_Quit(); SDL_Quit(); return false; }

        SDL_SetRenderDrawBlendMode(sRenderer, SDL_BLENDMODE_BLEND);

        sFont = TTF_OpenFontRW(SDL_RWFromConstMem(DejaVuSans_ttf, DejaVuSans_ttf_size), 0, 32);

        if (!sFont && OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &sFontData, &sFontSize)) {
            sFont = TTF_OpenFontRW(SDL_RWFromMem(sFontData, sFontSize), 0, 32);
        }

        if (OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &sFontData, &sFontSize)) {
            sIconFont = TTF_OpenFontRW(SDL_RWFromMem(sFontData, sFontSize), 0, 256);
        }

        return true;
    }

    void Shutdown() {
        OSReport("[EXIT] Gfx::Shutdown begin\n");

        if (sFont) {
            OSReport("[EXIT] TTF_CloseFont sFont\n");
            TTF_CloseFont(sFont);
            sFont = nullptr;
        }
        if (sIconFont) {
            OSReport("[EXIT] TTF_CloseFont sIconFont\n");
            TTF_CloseFont(sIconFont);
            sIconFont = nullptr;
        }

        OSReport("[EXIT] Closing %zu icon fonts by size\n", sIconFontBySize.size());
        for (auto& [key, fnt] : sIconFontBySize) TTF_CloseFont(fnt);
        sIconFontBySize.clear();
        OSReport("[EXIT] Icon fonts closed\n");

        if (sRenderer) {
            OSReport("[EXIT] SDL_DestroyRenderer\n");
            SDL_DestroyRenderer(sRenderer);
            sRenderer = nullptr;
            OSReport("[EXIT] SDL_DestroyRenderer done\n");
        }
        if (sWindow) {
            OSReport("[EXIT] SDL_DestroyWindow\n");
            SDL_DestroyWindow(sWindow);
            sWindow = nullptr;
            OSReport("[EXIT] SDL_DestroyWindow done\n");
        }

        OSReport("[EXIT] IMG_Quit\n");
        IMG_Quit();
        OSReport("[EXIT] TTF_Quit\n");
        TTF_Quit();
        OSReport("[EXIT] SDL_Quit\n");
        SDL_Quit();
        OSReport("[EXIT] Gfx::Shutdown complete\n");
    }

    void Clear(SDL_Color color) {
        SDL_SetRenderDrawColor(sRenderer, color.r, color.g, color.b, color.a);
        SDL_RenderClear(sRenderer);
    }

    void ClearGradient(SDL_Color topColor, SDL_Color bottomColor) {
        for (int y = 0; y < (int)SCREEN_HEIGHT; y++) {
            float r = (float)y / SCREEN_HEIGHT;
            SDL_Color c = {
                (uint8_t)(topColor.r + (bottomColor.r - topColor.r) * r),
                (uint8_t)(topColor.g + (bottomColor.g - topColor.g) * r),
                (uint8_t)(topColor.b + (bottomColor.b - topColor.b) * r),
                255
            };
            SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
            SDL_RenderDrawLine(sRenderer, 0, y, SCREEN_WIDTH, y);
        }
    }

    void Render() {
        SDL_RenderPresent(sRenderer);
    }

    void DrawRectFilled(int x, int y, int w, int h, SDL_Color color) {
        SDL_Rect rect = {x, y, w, h};
        SDL_SetRenderDrawColor(sRenderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(sRenderer, &rect);
    }

    void DrawRectOutline(int x, int y, int w, int h, SDL_Color color, int thickness) {
        SDL_SetRenderDrawColor(sRenderer, color.r, color.g, color.b, color.a);
        for (int i = 0; i < thickness; i++) {
            SDL_Rect rect = {x + i, y + i, w - 2*i, h - 2*i};
            SDL_RenderDrawRect(sRenderer, &rect);
        }
    }

    void DrawRectRounded(int x, int y, int w, int h, int radius, SDL_Color color) {
        SDL_SetRenderDrawColor(sRenderer, color.r, color.g, color.b, color.a);
        SDL_Rect rects[3] = {
            {x + radius, y,          w - 2*radius, h},
            {x,          y + radius, radius,        h - 2*radius},
            {x + w - radius, y + radius, radius,    h - 2*radius}
        };
        SDL_RenderFillRects(sRenderer, rects, 3);
        for (int dy = 0; dy < radius; dy++) {
            int dx = (int)std::sqrt((float)(radius*radius - dy*dy));
            SDL_RenderDrawLine(sRenderer, x + radius - dx, y + radius - dy, x + radius, y + radius - dy);
            SDL_RenderDrawLine(sRenderer, x + w - radius, y + radius - dy, x + w - radius + dx, y + radius - dy);
            SDL_RenderDrawLine(sRenderer, x + radius - dx, y + h - radius + dy, x + radius, y + h - radius + dy);
            SDL_RenderDrawLine(sRenderer, x + w - radius, y + h - radius + dy, x + w - radius + dx, y + h - radius + dy);
        }
    }

    void DrawRectRoundedOutline(int x, int y, int w, int h, int radius, SDL_Color color, int thickness) {
        // Rounded-rect outline by drawing concentric rounded rects
        for (int i = 0; i < thickness; i++) {
            // Using a approximate with a regular outline for simplicity and to make it look purdy ("?"'_:_'"?")
            DrawRectOutline(x + i, y + i, w - 2*i, h - 2*i, color, 1);
        }
    }

    void DrawRectGradient(int x, int y, int w, int h, SDL_Color topColor, SDL_Color bottomColor) {
        for (int i = 0; i < h; i++) {
            float r = (float)i / h;
            SDL_Color c = {
                (uint8_t)(topColor.r + (bottomColor.r - topColor.r) * r),
                (uint8_t)(topColor.g + (bottomColor.g - topColor.g) * r),
                (uint8_t)(topColor.b + (bottomColor.b - topColor.b) * r),
                (uint8_t)(topColor.a + (bottomColor.a - topColor.a) * r)
            };
            SDL_SetRenderDrawColor(sRenderer, c.r, c.g, c.b, c.a);
            SDL_RenderDrawLine(sRenderer, x, y + i, x + w, y + i);
        }
    }

    void DrawCircleFilled(int cx, int cy, int radius, SDL_Color color) {
        SDL_SetRenderDrawColor(sRenderer, color.r, color.g, color.b, color.a);
        for (int y = -radius; y <= radius; y++) {
            int x = (int)std::sqrt((float)(radius*radius - y*y));
            SDL_RenderDrawLine(sRenderer, cx - x, cy + y, cx + x, cy + y);
        }
    }

    void Print(int x, int y, int size, SDL_Color color, const std::string& text, AlignFlags align) {
        if (!sFont || text.empty()) return;

        SDL_Surface* surface = TTF_RenderUTF8_Blended(sFont, text.c_str(), color);
        if (!surface) return;

        SDL_Texture* texture = SDL_CreateTextureFromSurface(sRenderer, surface);
        int w = surface->w, h = surface->h;
        SDL_FreeSurface(surface);
        if (!texture) return;

        if (align & ALIGN_HORIZONTAL) x -= w / 2;
        else if (align & ALIGN_RIGHT) x -= w;
        if (align & ALIGN_VERTICAL)   y -= h / 2;
        else if (align & ALIGN_BOTTOM) y -= h;

        SDL_Rect dst = {x, y, w, h};
        SDL_RenderCopy(sRenderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }

    void PrintWithShadow(int x, int y, int size, SDL_Color color, const std::string& text, AlignFlags align) {
        Print(x + 2, y + 2, size, T.TEXT_SHADOW, text, align);
        Print(x, y, size, color, text, align);
    }

    static TTF_Font* GetIconFontForSize(int size) {
        auto it = sIconFontBySize.find(size);
        if (it != sIconFontBySize.end()) return it->second;
        if (!sFontData) return nullptr;
        TTF_Font* f = TTF_OpenFontRW(SDL_RWFromMem(sFontData, sFontSize), 0, size);
        if (f) sIconFontBySize[size] = f;
        return f;
    }

    void PrintIcon(int x, int y, int size, SDL_Color color, const std::string& text, AlignFlags align) {
        TTF_Font* font = GetIconFontForSize(size);
        if (!font || text.empty()) return;

        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!surface) return;

        SDL_Texture* texture = SDL_CreateTextureFromSurface(sRenderer, surface);
        int w = surface->w, h = surface->h;
        SDL_FreeSurface(surface);
        if (!texture) return;

        if (align & ALIGN_HORIZONTAL) x -= w / 2;
        else if (align & ALIGN_RIGHT) x -= w;
        if (align & ALIGN_VERTICAL)   y -= h / 2;
        else if (align & ALIGN_BOTTOM) y -= h;

        SDL_Rect dst = {x, y, w, h};
        SDL_RenderCopy(sRenderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }

    int GetIconTextWidth(int size, const std::string& text) {
        TTF_Font* font = GetIconFontForSize(size);
        if (!font || text.empty()) return 0;
        int w = 0;
        TTF_SizeUTF8(font, text.c_str(), &w, nullptr);
        return w;
    }

    int GetTextWidth(int size, const std::string& text) {
        if (!sFont || text.empty()) return 0;
        int w = 0;
        TTF_SizeUTF8(sFont, text.c_str(), &w, nullptr);
        return w;
    }

    int GetTextHeight(int size, const std::string& text) {
        if (!sFont || text.empty()) return 0;
        int h = 0;
        TTF_SizeUTF8(sFont, text.c_str(), nullptr, &h);
        return h;
    }

    SDL_Texture* LoadTexture(const std::string& path) {
        SDL_Surface* surface = IMG_Load(path.c_str());
        if (!surface) return nullptr;
        SDL_Texture* texture = SDL_CreateTextureFromSurface(sRenderer, surface);
        SDL_FreeSurface(surface);
        return texture;
    }

    void DrawTexture(SDL_Texture* texture, int x, int y, int w, int h, uint8_t alpha) {
        if (!texture) return;
        SDL_SetTextureAlphaMod(texture, alpha);
        SDL_Rect dst = {x, y, w, h};
        SDL_RenderCopy(sRenderer, texture, nullptr, &dst);
    }

    SDL_Renderer* GetRenderer() { return sRenderer; }

    uint32_t GetTicks() { return SDL_GetTicks(); }
}
