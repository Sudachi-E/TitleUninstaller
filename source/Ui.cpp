#include "TitleUninstaller.hpp"
#include "TitleDefs.hpp"
#include "Gfx.hpp"
#include "Input.hpp"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cinttypes>

namespace {

void DrawAppHeader(const char* suffix) {
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, 120, Gfx::COLOR_BAR_BG());
    Gfx::DrawRectFilled(0, 120, Gfx::SCREEN_WIDTH, 4, Gfx::COLOR_ACCENT_DARK());
    Gfx::PrintWithShadow(60, 60, 52, Gfx::COLOR_WHITE, "Title Uninstaller",
                         Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    if (suffix) {
        int titleW = Gfx::GetTextWidth(52, "Title Uninstaller");
        Gfx::Print(60 + titleW + 24, 60, 28, Gfx::COLOR_ACCENT_DARK(), suffix,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }
}

void DrawHint(int x, const char* glyph, const char* label, Gfx::AlignFlags align) {
    constexpr int ICON_SZ = 34;
    constexpr int TXT_SZ  = 26;
    constexpr int GAP     = 6;
    constexpr int Y       = 1040;

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
}

void DrawButtonWithIcon(int x, int y, int w, int h, SDL_Color bg,
                        const char* glyph, const char* label, SDL_Color fg) {
    constexpr int ICON_SZ = 30;
    constexpr int TXT_SZ  = 26;
    constexpr int GAP     = 6;

    Gfx::DrawRectRounded(x, y, w, h, 12, bg);
    int iw = Gfx::GetIconTextWidth(ICON_SZ, glyph);
    int tw = Gfx::GetTextWidth(TXT_SZ, label);
    int total = iw + GAP + tw;
    int sx = x + w / 2 - total / 2;
    Gfx::PrintIcon(sx, y + h / 2, ICON_SZ, fg, glyph,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::Print(sx + iw + GAP, y + h / 2, TXT_SZ, fg, label,
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
}

void DrawSelectionRow(int x, int y, int w, int h, float pulse) {
    uint8_t alpha = (uint8_t)(180 * pulse + 75);
    SDL_Color border = {Gfx::COLOR_ACCENT().r, Gfx::COLOR_ACCENT().g,
                        Gfx::COLOR_ACCENT().b, alpha};
    Gfx::DrawRectOutline(x, y, w, h, border, 3);
    Gfx::DrawRectFilled(x, y, 6, h, Gfx::COLOR_ACCENT());
}

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

void TitleUninstaller::DrawBackground() {
    Gfx::ClearGradient(Gfx::COLOR_BG_TOP(), Gfx::COLOR_BG_BOTTOM());
}

void TitleUninstaller::DrawTopBar() {
    DrawAppHeader(nullptr);

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

    int hintX = tabX + TAB_W + 8 + TAB_W + 20;
    int iw1 = Gfx::GetIconTextWidth(28, "\xee\x82\x85");
    int iw2 = Gfx::GetIconTextWidth(28, "\xee\x82\x86");
    constexpr int HINT_GAP = 4;
    Gfx::PrintIcon(hintX, 60, 28, Gfx::COLOR_ACCENT_DARK(),
                   "\xee\x82\x85", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::PrintIcon(hintX + iw1 + 2, 60, 28, Gfx::COLOR_ACCENT_DARK(),
                   "\xee\x82\x86", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::Print(hintX + iw1 + 2 + iw2 + HINT_GAP, 60, 22, Gfx::COLOR_WHITE,
               "Switch", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    const char* sortLabel = "A\xe2\x80\x93Z";
    if (sortMode == SortMode::SizeDesc) sortLabel = "Size \xe2\x86\x93";
    if (sortMode == SortMode::SizeAsc)  sortLabel = "Size \xe2\x86\x91";
    std::string sortStr = std::string("Sort: ") + sortLabel;
    int sw = Gfx::GetTextWidth(26, sortStr);
    constexpr int PILL_PAD = 20;
    constexpr int PILL_H   = 36;
    int pillX = Gfx::SCREEN_WIDTH - 60 - sw - PILL_PAD;
    int pillY = 60 - PILL_H / 2;

    {
        int lrIw1 = Gfx::GetIconTextWidth(28, "\xee\x82\x83");
        int lrIw2 = Gfx::GetIconTextWidth(28, "\xee\x82\x84");
        int lrTw  = Gfx::GetTextWidth(22, "Sort");
        int lrTotal = lrIw1 + 2 + lrIw2 + 4 + lrTw;
        int lrX = pillX - lrTotal - 16;
        Gfx::PrintIcon(lrX, 60, 28, Gfx::COLOR_ACCENT_DARK(),
                       "\xee\x82\x83", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::PrintIcon(lrX + lrIw1 + 2, 60, 28, Gfx::COLOR_ACCENT_DARK(),
                       "\xee\x82\x84", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(lrX + lrIw1 + 2 + lrIw2 + 4, 60, 22, Gfx::COLOR_WHITE,
                   "Sort", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }

    Gfx::DrawRectRounded(pillX, pillY, sw + PILL_PAD * 2, PILL_H, 10,
                         Gfx::COLOR_ACCENT_DARK());
    Gfx::Print(pillX + PILL_PAD + sw / 2, 60, 26, Gfx::COLOR_WHITE,
               sortStr, Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_VERTICAL);
}

void TitleUninstaller::DrawBottomBar() {
    Gfx::DrawRectFilled(0, 1000, Gfx::SCREEN_WIDTH, 80, Gfx::COLOR_BAR_BOTTOM());
    Gfx::DrawRectFilled(0, 1000, Gfx::SCREEN_WIDTH, 3, Gfx::COLOR_ACCENT());

    int checked = CheckedCount();
    int total   = (int)titles.size();
    char countBuf[64];
    snprintf(countBuf, sizeof(countBuf), "Selected %d / %d", checked, total);
    int cw2 = Gfx::GetTextWidth(26, countBuf);
    Gfx::DrawRectRounded(40, 1040 - 16, cw2 + 24, 32, 8, Gfx::COLOR_ACCENT());
    Gfx::Print(52, 1040, 26, Gfx::COLOR_WHITE, countBuf,
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    int settingsX = 40 + cw2 + 24 + 20;
    DrawHint(settingsX, "\xee\x81\x86", "Settings", Gfx::ALIGN_LEFT);

    DrawHint(Gfx::SCREEN_WIDTH / 2, "\xee\x81\x84", "Exit", Gfx::ALIGN_HORIZONTAL);

    DrawHint(Gfx::SCREEN_WIDTH / 2 - 170, "\xee\x80\x82", "Refresh",
             Gfx::ALIGN_RIGHT);

    DrawHint(1920 - 40,       "\xee\x80\x80", "Select",          Gfx::ALIGN_RIGHT);
    DrawHint(1920 - 40 - 220, "\xee\x80\x83", "All/None",        Gfx::ALIGN_RIGHT);
    DrawHint(1920 - 40 - 440, "\xee\x81\x85", "Delete Selected", Gfx::ALIGN_RIGHT);
}

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

        Gfx::DrawRectFilled(x + 3, y + 3, LIST_W - 6, ROW_H - 6,
                            {0x00, 0x00, 0x00, 0x18});

        SDL_Color rowBg = sel ? Gfx::COLOR_ROW_SELECTED()
                              : (t.checked ? Gfx::COLOR_ROW_CHECKED() : Gfx::COLOR_ROW_BG());
        Gfx::DrawRectRounded(x + 2, y + 2, LIST_W - 4, ROW_H - 6, 10, rowBg);

        if (sel) {
            float pulse = std::sin(selectionPulse) * 0.3f + 0.7f;
            DrawSelectionRow(x + 2, y + 2, LIST_W - 4, ROW_H - 6, pulse);
        }

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

        int textX = iconX + ICON_SIZE + 20;
        Gfx::Print(textX, y + ROW_H / 2, 32, Gfx::COLOR_TEXT(), t.name,
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

        std::string sizeStr = FormatSize(t.sizeBytes);
        Gfx::Print(LIST_X + LIST_W - 24, y + ROW_H / 2, 30,
                   Gfx::COLOR_SIZE_TEXT(), sizeStr,
                   Gfx::ALIGN_RIGHT | Gfx::ALIGN_VERTICAL);
    }

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

void TitleUninstaller::DrawStoragePanel() {
    int px = PANEL_X;
    int py = PANEL_Y;
    int pw = PANEL_W;

    Gfx::DrawRectFilled(px + 4, py + 4, pw, 360, {0x00, 0x00, 0x00, 0x18});
    Gfx::DrawRectRounded(px, py, pw, 360, 14, Gfx::COLOR_PANEL_BG());
    Gfx::DrawRectOutline(px, py, pw, 360, Gfx::COLOR_SEPARATOR(), 1);

    Gfx::DrawRectRounded(px, py, pw, 52, 14, Gfx::COLOR_ACCENT());
    Gfx::DrawRectFilled(px, py + 38, pw, 14, Gfx::COLOR_ACCENT());
    Gfx::Print(px + 20, py + 26, 26, Gfx::COLOR_WHITE,
               (currentStorage == StorageLocation::USB) ? "USB Storage" : "NAND Storage",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    int barX = px + 20;
    int barY = py + 80;
    int barW = pw - 40;
    int barH = 20;
    Gfx::DrawRectRounded(barX, barY, barW, barH, barH / 2, Gfx::COLOR_STORAGE_FREE());

    int usedW = 0;
    if (storageTotalBytes > 0) {
        usedW = (int)((float)barW * (1.0f - (float)storageFreeBytes / storageTotalBytes));
        if (usedW > 0)
            Gfx::DrawRectRounded(barX, barY, usedW, barH, barH / 2, Gfx::COLOR_STORAGE_USED());
    } else {
        usedW = barW / 2;
        Gfx::DrawRectRounded(barX, barY, usedW, barH, barH / 2, Gfx::COLOR_STORAGE_USED());
    }

    if (storageTotalBytes > 0 && highlightBarAnim > 0.001f) {
        int freeW = (int)(highlightBarAnim * barW);
        freeW = std::max(4, std::min(freeW, barW - usedW));
        float blink = std::sin(selectionPulse * 2.0f) * 0.5f + 0.5f;
        uint8_t alpha = (uint8_t)(120 + blink * 135);
        SDL_Color green = {0x44, 0xff, 0x88, alpha};
        Gfx::DrawRectFilled(barX + usedW, barY, freeW, barH, green);
    }

    Gfx::Print(px + 20, barY + barH + 12, 26, Gfx::COLOR_TEXT_DIM(), "Space available",
               Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);
    std::string freeStr = (storageFreeBytes > 0) ? FormatSize(storageFreeBytes) : "N/A";
    Gfx::Print(px + pw - 20, barY + barH + 12, 26, Gfx::COLOR_TEXT(), freeStr,
               Gfx::ALIGN_RIGHT | Gfx::ALIGN_TOP);

    Gfx::DrawRectFilled(px + 20, py + 150, pw - 40, 1, Gfx::COLOR_SEPARATOR());

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

    Gfx::DrawRectFilled(px + 20, py + 305, pw - 40, 1, Gfx::COLOR_SEPARATOR());
    char totalBuf[48];
    snprintf(totalBuf, sizeof(totalBuf), "%d %s title%s installed",
             (int)titles.size(),
             (currentStorage == StorageLocation::USB) ? "USB" : "NAND",
             titles.size() == 1 ? "" : "s");
    Gfx::Print(px + 20, py + 320, 24, Gfx::COLOR_TEXT_DIM(), totalBuf,
               Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);
}

void TitleUninstaller::DrawConfirmDialog() {
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT, {0x00, 0x00, 0x00, 0xb0});

    int dw = 860, dh = 360;
    int dx = (Gfx::SCREEN_WIDTH  - dw) / 2;
    int dy = (Gfx::SCREEN_HEIGHT - dh) / 2;

    Gfx::DrawRectFilled(dx + 5, dy + 5, dw, dh, {0x00, 0x00, 0x00, 0x30});
    Gfx::DrawRectRounded(dx, dy, dw, dh, 20, Gfx::COLOR_PANEL_BG());

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
    DrawButtonWithIcon(dx + 80, btnY, 280, 52, Gfx::COLOR_DANGER(),
                       "\xee\x80\x80", "Confirm", Gfx::COLOR_WHITE);
    DrawButtonWithIcon(dx + dw - 360, btnY, 280, 52, Gfx::COLOR_SEPARATOR(),
                       "\xee\x80\x81", "Cancel", Gfx::COLOR_TEXT());
}

void TitleUninstaller::DrawComponentSelectDialog() {
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT, {0x00, 0x00, 0x00, 0xb8});

    constexpr int DW        = 1360;
    constexpr int HEADER_H  = 64;
    constexpr int FOOTER_H  = 90;
    constexpr int COL_HDR_H = 40;
    constexpr int ROW_H     = 92;
    constexpr int ROW_PAD   = 6;
    constexpr int MAX_ROWS  = 6;

    int numRows  = (int)componentChoices.size();
    int visRows  = std::min(numRows, MAX_ROWS);
    int DH       = HEADER_H + COL_HDR_H + visRows * (ROW_H + ROW_PAD) + ROW_PAD + FOOTER_H + 8;

    int dx = (Gfx::SCREEN_WIDTH  - DW) / 2;
    int dy = (Gfx::SCREEN_HEIGHT - DH) / 2;

    Gfx::DrawRectFilled(dx + 6, dy + 6, DW, DH, {0x00, 0x00, 0x00, 0x38});
    Gfx::DrawRectRounded(dx, dy, DW, DH, 20, Gfx::COLOR_PANEL_BG());

    Gfx::DrawRectRounded(dx, dy, DW, HEADER_H, 20, Gfx::COLOR_ACCENT());
    Gfx::DrawRectFilled(dx, dy + HEADER_H - 20, DW, 20, Gfx::COLOR_ACCENT());

    int titleW = Gfx::GetTextWidth(32, "Select Components to Delete");
    int totalTitleW = + 10 + titleW;
    int titleStartX = dx + DW / 2 - totalTitleW / 2;
    Gfx::Print(titleStartX + 10, dy + HEADER_H / 2, 32, Gfx::COLOR_WHITE,
               "Select Components to Delete", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);

    {
        char badge[16];
        snprintf(badge, sizeof(badge), "%d", numRows);
        int bw = Gfx::GetTextWidth(22, badge) + 20;
        int bh = 28;
        int bx = dx + DW - 24 - bw;
        int by = dy + HEADER_H / 2 - bh / 2;
        Gfx::DrawRectRounded(bx, by, bw, bh, bh / 2, {0x00,0x00,0x00,0x40});
        Gfx::Print(bx + bw / 2, by + bh / 2, 22, Gfx::COLOR_WHITE,
                   badge, Gfx::ALIGN_CENTER | Gfx::ALIGN_VERTICAL);
    }

    constexpr int PAD       = 24;
    constexpr int ICON_SZ   = 52;
    constexpr int COL_W     = 160;
    constexpr int CB_R      = 13;

    int contentX  = dx + PAD;
    int contentW  = DW - PAD * 2;

    int dlcCX  = dx + DW - PAD - COL_W / 2;
    int updCX  = dlcCX - COL_W;
    int gameCX = updCX - COL_W;

    int nameCX = contentX + ICON_SZ + 16;
    int nameMaxW = gameCX - COL_W / 2 - nameCX - 16;

    int hdrY = dy + HEADER_H;
    Gfx::DrawRectFilled(dx, hdrY, DW, COL_HDR_H, {0x00,0x00,0x00,0x28});

    Gfx::Print(nameCX, hdrY + COL_HDR_H / 2, 22, Gfx::COLOR_TEXT_DIM(),
               "Game Title", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::Print(gameCX, hdrY + COL_HDR_H / 2, 22, Gfx::COLOR_TEXT_DIM(),
               "Game", Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_VERTICAL);
    Gfx::Print(updCX,  hdrY + COL_HDR_H / 2, 22, Gfx::COLOR_TEXT_DIM(),
               "Update", Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_VERTICAL);
    Gfx::Print(dlcCX,  hdrY + COL_HDR_H / 2, 22, Gfx::COLOR_TEXT_DIM(),
               "DLC", Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_VERTICAL);

    SDL_Color div = Gfx::COLOR_SEPARATOR();
    int divTop = hdrY + 6;
    int divBot = hdrY + COL_HDR_H + visRows * (ROW_H + ROW_PAD);
    Gfx::DrawRectFilled(gameCX - COL_W / 2 - 1, divTop, 1, divBot - divTop, div);
    Gfx::DrawRectFilled(updCX  - COL_W / 2 - 1, divTop, 1, divBot - divTop, div);
    Gfx::DrawRectFilled(dlcCX  - COL_W / 2 - 1, divTop, 1, divBot - divTop, div);

    Gfx::DrawRectFilled(dx + PAD, hdrY + COL_HDR_H - 1, contentW, 1, Gfx::COLOR_SEPARATOR());

    int listTop = hdrY + COL_HDR_H;
    int listBot = listTop + visRows * (ROW_H + ROW_PAD) + ROW_PAD;

    SDL_Rect clip = { dx, listTop, DW, listBot - listTop };
    SDL_RenderSetClipRect(Gfx::GetRenderer(), &clip);

    int scrollOff = 0;
    if (numRows > MAX_ROWS) {
        scrollOff = componentFocusIdx - MAX_ROWS / 2;
        if (scrollOff < 0) scrollOff = 0;
        if (scrollOff > numRows - MAX_ROWS) scrollOff = numRows - MAX_ROWS;
    }

    for (int i = 0; i < numRows; i++) {
        int visIdx = i - scrollOff;
        if (visIdx < 0 || visIdx >= MAX_ROWS) continue;

        int slotY  = listTop + visIdx * (ROW_H + ROW_PAD) + ROW_PAD;
        int cardH  = ROW_H;
        int y      = slotY;

        bool focused = (i == componentFocusIdx);
        const ComponentChoice& cc = componentChoices[i];
        TitleEntry& t = *titles[cc.titleIdx];
        uint32_t lowId = (uint32_t)(t.titleId & 0xFFFFFFFF);
        bool hasUpdate = updateMap.count(lowId) > 0;
        bool hasDLC    = dlcMap.count(lowId) > 0;

        SDL_Color rowBg = focused
            ? Gfx::COLOR_ROW_SELECTED()
            : (i % 2 == 0 ? Gfx::COLOR_ROW_BG() : Gfx::COLOR_ROW_BG_ALT());
        Gfx::DrawRectFilled(dx + 4, y + 2, DW - 8, cardH - 2, {0,0,0,0x18});
        Gfx::DrawRectRounded(dx + 4, y, DW - 8, cardH, 10, rowBg);

        if (focused) {
            float pulse = std::sin(selectionPulse) * 0.3f + 0.7f;
            uint8_t alpha = (uint8_t)(180 * pulse + 75);
            SDL_Color border = {Gfx::COLOR_ACCENT().r, Gfx::COLOR_ACCENT().g,
                                Gfx::COLOR_ACCENT().b, alpha};
            Gfx::DrawRectRounded(dx + 4, y, 6, cardH, 3, Gfx::COLOR_ACCENT());
            Gfx::DrawRectOutline(dx + 4, y, DW - 8, cardH, border, 2);
        }

        int iconX = contentX + 8;
        int iconY = y + (cardH - ICON_SZ) / 2;
        if (t.icon) {
            Gfx::DrawRectRounded(iconX - 2, iconY - 2, ICON_SZ + 4, ICON_SZ + 4,
                                 6, Gfx::COLOR_SEPARATOR());
            Gfx::DrawTexture(t.icon, iconX, iconY, ICON_SZ, ICON_SZ);
        } else {
            Gfx::DrawRectRounded(iconX, iconY, ICON_SZ, ICON_SZ, 6,
                                 Gfx::COLOR_SEPARATOR());
            Gfx::Print(iconX + ICON_SZ / 2, iconY + ICON_SZ / 2, 22,
                       Gfx::COLOR_TEXT_DIM(), "?", Gfx::ALIGN_CENTER);
        }

        std::string displayName = t.name;
        while (displayName.size() > 4 &&
               Gfx::GetTextWidth(28, displayName) > nameMaxW) {
            displayName.resize(displayName.size() - 1);
        }
        if (displayName != t.name) displayName += "\xe2\x80\xa6";

        int nameY = y + cardH / 2 - 22;
        int sizeY = y + cardH / 2 + 14;

        Gfx::Print(nameCX, nameY, 28, Gfx::COLOR_TEXT(),
                   displayName, Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);

        std::string sizeStr = FormatSize(t.sizeBytes);
        Gfx::Print(nameCX, sizeY, 20, Gfx::COLOR_TEXT_DIM(),
                   sizeStr, Gfx::ALIGN_LEFT | Gfx::ALIGN_TOP);

        int cbY    = y + cardH / 2 - 10;
        int cbSzY  = y + cardH / 2 + 14;

        auto drawComponentCell = [&](int cx, bool checked, bool enabled,
                                     int compIdx, uint64_t compSize) {
            bool compFocused = focused && (compIdx == componentFocusComponent);
            int cy = cbY;

            if (!enabled) {
                SDL_Color naCol = {0x55, 0x55, 0x66, 0xff};
                Gfx::DrawRectFilled(cx - 12, cy - 1, 24, 2, naCol);
                Gfx::Print(cx, cbSzY, 18, naCol,
                           "N/A", Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_TOP);
                return;
            }

            SDL_Color ringCol;
            if (compFocused)
                ringCol = Gfx::COLOR_ACCENT_LIGHT();
            else if (checked)
                ringCol = Gfx::COLOR_ACCENT();
            else
                ringCol = Gfx::COLOR_SEPARATOR();

            Gfx::DrawCircleFilled(cx, cy, CB_R, ringCol);

            if (checked) {
                Gfx::DrawCircleFilled(cx, cy, CB_R - 4, Gfx::COLOR_WHITE);
            } else {
                Gfx::DrawCircleFilled(cx, cy, CB_R - 3,
                                      focused ? Gfx::COLOR_ROW_SELECTED()
                                              : Gfx::COLOR_ROW_BG());
            }

            if (compFocused) {
                float pulse = std::sin(selectionPulse * 1.5f) * 0.4f + 0.6f;
                uint8_t a = (uint8_t)(200 * pulse);
                SDL_Color hl = {Gfx::COLOR_ACCENT_LIGHT().r,
                                Gfx::COLOR_ACCENT_LIGHT().g,
                                Gfx::COLOR_ACCENT_LIGHT().b, a};
                Gfx::DrawCircleFilled(cx, cy, CB_R + 4,
                                      {hl.r, hl.g, hl.b, (uint8_t)(a / 3)});
            }

            if (compSize > 0) {
                std::string cs = FormatSize(compSize);
                Gfx::Print(cx, cbSzY, 20,
                           checked ? Gfx::COLOR_ACCENT() : Gfx::COLOR_TEXT_DIM(),
                           cs, Gfx::ALIGN_HORIZONTAL | Gfx::ALIGN_TOP);
            }
        };

        uint64_t updSize = 0, dlcSize = 0;
        if (hasUpdate) updSize = updateMap[lowId]->sizeBytes;
        if (hasDLC)    dlcSize = dlcMap[lowId]->sizeBytes;

        drawComponentCell(gameCX, cc.wantGame,   true,      0, t.sizeBytes);
        drawComponentCell(updCX,  cc.wantUpdate, hasUpdate, 1, updSize);
        drawComponentCell(dlcCX,  cc.wantDLC,    hasDLC,    2, dlcSize);
    }

    SDL_RenderSetClipRect(Gfx::GetRenderer(), nullptr);

    if (numRows > MAX_ROWS) {
        int trackX = dx + DW - 10;
        int trackH = visRows * (ROW_H + ROW_PAD);
        Gfx::DrawRectRounded(trackX, listTop, 6, trackH, 3, Gfx::COLOR_SEPARATOR());
        float ratio = (float)scrollOff / (numRows - MAX_ROWS);
        int thumbH  = std::max(30, trackH * MAX_ROWS / numRows);
        int thumbY  = listTop + (int)(ratio * (trackH - thumbH));
        Gfx::DrawRectRounded(trackX, thumbY, 6, thumbH, 3, Gfx::COLOR_ACCENT());
    }

    Gfx::DrawRectFilled(dx, listBot, DW, 1, Gfx::COLOR_SEPARATOR());

    int footerY = listBot + 1;

    Gfx::DrawRectFilled(dx, footerY, DW, 36, {0x00,0x00,0x00,0x20});

    struct HintItem { const char* glyph; const char* label; };
    HintItem hints[] = {
        { "\xee\x81\xbd", "Navigate" },
        { "\xee\x81\xbe", "Switch column" },
        { "\xee\x80\x80", "Toggle" },
    };
    constexpr int HINT_ICON_SZ = 26;
    constexpr int HINT_TXT_SZ  = 20;
    constexpr int HINT_GAP     = 5;
    constexpr int HINT_SEP     = 36;
    int hintY = footerY + 18;

    int totalHintW = 0;
    for (auto& h : hints) {
        totalHintW += Gfx::GetIconTextWidth(HINT_ICON_SZ, h.glyph)
                    + HINT_GAP
                    + Gfx::GetTextWidth(HINT_TXT_SZ, h.label);
    }
    totalHintW += HINT_SEP * 2;

    int hx = dx + DW / 2 - totalHintW / 2;
    for (int h = 0; h < 3; h++) {
        int iw = Gfx::GetIconTextWidth(HINT_ICON_SZ, hints[h].glyph);
        Gfx::PrintIcon(hx, hintY, HINT_ICON_SZ, Gfx::COLOR_ACCENT_DARK(),
                       hints[h].glyph, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        Gfx::Print(hx + iw + HINT_GAP, hintY, HINT_TXT_SZ, Gfx::COLOR_TEXT_DIM(),
                   hints[h].label, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        hx += iw + HINT_GAP + Gfx::GetTextWidth(HINT_TXT_SZ, hints[h].label) + HINT_SEP;
    }

    constexpr int BTN_W = 260;
    constexpr int BTN_H = 52;
    int btnY = footerY + 36 + (FOOTER_H - 36 - BTN_H) / 2;

    {
        uint64_t totalSel = 0;
        for (const auto& cc : componentChoices) {
            TitleEntry& t = *titles[cc.titleIdx];
            uint32_t lid = (uint32_t)(t.titleId & 0xFFFFFFFF);
            if (cc.wantGame)   totalSel += t.sizeBytes;
            if (cc.wantUpdate && updateMap.count(lid)) totalSel += updateMap[lid]->sizeBytes;
            if (cc.wantDLC    && dlcMap.count(lid))    totalSel += dlcMap[lid]->sizeBytes;
        }
        std::string sumStr = FormatSize(totalSel) + " will be freed";
        Gfx::Print(dx + DW / 2, btnY + BTN_H / 2, 24,
                   Gfx::COLOR_ACCENT(), sumStr,
                   Gfx::ALIGN_CENTER | Gfx::ALIGN_VERTICAL);
    }

    DrawButtonWithIcon(dx + 48, btnY, BTN_W, BTN_H, Gfx::COLOR_DANGER(),
                       "\xee\x81\x85", "Delete", Gfx::COLOR_WHITE);

    DrawButtonWithIcon(dx + DW - 48 - BTN_W, btnY, BTN_W, BTN_H,
                       Gfx::COLOR_SEPARATOR(), "\xee\x80\x81", "Cancel",
                       Gfx::COLOR_TEXT());
}

void TitleUninstaller::DrawUninstallProgress() {
    DrawBackground();
    DrawTopBar();

    int total   = (int)uninstallQueue.size();
    int current = std::min(uninstallCurrent, total);

    Gfx::DrawRectFilled(203, 383, 1520, 314, {0x00, 0x00, 0x00, 0x18});
    Gfx::DrawRectRounded(200, 380, 1520, 314, 16, Gfx::COLOR_PANEL_BG());

    if (uninstallCurrent < total) {
        Gfx::Print(960, 430, 34, Gfx::COLOR_TEXT(),
                   "Uninstalling: " + uninstallQueue[uninstallCurrent].name, Gfx::ALIGN_CENTER);
    }

    int barX = 260, barY = 490, barW = 1400, barH = 24;
    Gfx::DrawRectRounded(barX, barY, barW, barH, barH / 2, Gfx::COLOR_STORAGE_FREE());
    if (total > 0) {
        int fillW = (int)((float)barW * current / total);
        if (fillW > 0)
            Gfx::DrawRectRounded(barX, barY, fillW, barH, barH / 2, Gfx::COLOR_ACCENT());
    }

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

void TitleUninstaller::DrawDoneScreen() {
    DrawBackground();
    DrawTopBar();

    Gfx::DrawRectFilled(363, 283, 1200, 420, {0x00, 0x00, 0x00, 0x18});
    Gfx::DrawRectRounded(360, 280, 1200, 420, 20, Gfx::COLOR_PANEL_BG());

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

void TitleUninstaller::DrawLoadingScreen() {
    DrawBackground();
    DrawTopBar();

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

void TitleUninstaller::DrawSettingsScreen() {
    DrawBackground();
    DrawAppHeader("/ Settings");

    int cw = 860, ch = 460;
    int cx = (Gfx::SCREEN_WIDTH  - cw) / 2;
    int cy = (Gfx::SCREEN_HEIGHT - ch) / 2;

    Gfx::DrawRectFilled(cx + 4, cy + 4, cw, ch, {0x00, 0x00, 0x00, 0x30});
    Gfx::DrawRectRounded(cx, cy, cw, ch, 20, Gfx::COLOR_PANEL_BG());
    Gfx::DrawRectOutline(cx, cy, cw, ch, Gfx::COLOR_SEPARATOR(), 1);

    constexpr int ROW_Y1  = 40;
    constexpr int ROW_H_S = 90;

    auto drawSettingRow = [&](int rowY, int itemIdx, const char* label,
                               const char* valueLabel) {
        bool sel = (settingsSelectedItem == itemIdx);
        SDL_Color rowBg = sel ? Gfx::COLOR_ROW_SELECTED() : Gfx::COLOR_ROW_BG();
        Gfx::DrawRectRounded(cx + 20, cy + rowY, cw - 40, ROW_H_S, 12, rowBg);
        if (sel) {
            float pulse = std::sin(selectionPulse) * 0.3f + 0.7f;
            DrawSelectionRow(cx + 20, cy + rowY, cw - 40, ROW_H_S, pulse);
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

    constexpr int ROW_Y2 = ROW_Y1 + ROW_H_S + 10;
    const char* keepAwakeLabel = keepAwake ? "On" : "Off";
    drawSettingRow(ROW_Y2, 1, "Keep Screen On", keepAwakeLabel);

    {
        constexpr int HINT_SZ = 26, HINT_ICON = 28, HINT_GAP = 5;
        int hy = cy + ROW_Y2 + ROW_H_S + 30;
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
    DrawHint(Gfx::SCREEN_WIDTH / 2, "\xee\x81\x84", "Exit (HOME)", Gfx::ALIGN_HORIZONTAL);
    DrawHint(1920 - 40,       "\xee\x80\x80", "Change", Gfx::ALIGN_RIGHT);
    DrawHint(1920 - 40 - 200, "\xee\x80\x81", "Back",   Gfx::ALIGN_RIGHT);
}
