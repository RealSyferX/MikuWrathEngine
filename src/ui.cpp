#include "ui.h"
#include "types.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ============================================================
// Fonts
// ============================================================
Gdiplus::Font* UI::g_font = nullptr;
Gdiplus::Font* UI::g_fontBold = nullptr;
Gdiplus::Font* UI::g_fontSmall = nullptr;
int UI::g_fontSize = 9;

static Gdiplus::FontFamily* s_fontFamily = nullptr;

void UI::InitFonts() {
    s_fontFamily = new Gdiplus::FontFamily(L"Consolas");
    if (!s_fontFamily->IsAvailable()) {
        delete s_fontFamily;
        s_fontFamily = new Gdiplus::FontFamily(L"Courier New");
    }
    g_font = new Gdiplus::Font(s_fontFamily, 9, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    g_fontBold = new Gdiplus::Font(s_fontFamily, 9, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    g_fontSmall = new Gdiplus::Font(s_fontFamily, 8, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
}

void UI::CleanupFonts() {
    delete g_font; g_font = nullptr;
    delete g_fontBold; g_fontBold = nullptr;
    delete g_fontSmall; g_fontSmall = nullptr;
    delete s_fontFamily; s_fontFamily = nullptr;
}

void UI::RecreateFonts(int size) {
    g_fontSize = size;
    delete g_font; g_font = nullptr;
    delete g_fontBold; g_fontBold = nullptr;
    delete g_fontSmall; g_fontSmall = nullptr;
    // Reuse the existing font family if possible
    if (!s_fontFamily) {
        s_fontFamily = new Gdiplus::FontFamily(L"Consolas");
        if (!s_fontFamily->IsAvailable()) {
            delete s_fontFamily;
            s_fontFamily = new Gdiplus::FontFamily(L"Courier New");
        }
    }
    int smallSize = (size - 1 > 6) ? size - 1 : 6;
    g_font = new Gdiplus::Font(s_fontFamily, (float)size, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    g_fontBold = new Gdiplus::Font(s_fontFamily, (float)size, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    g_fontSmall = new Gdiplus::Font(s_fontFamily, (float)smallSize, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
}

// ============================================================
// Primitive drawing
// ============================================================
void UI::FillRect(Gdiplus::Graphics* g, const RECT& rc, const Gdiplus::Color& color) {
    if (!g) return;
    Gdiplus::SolidBrush brush(color);
    g->FillRectangle(&brush, (int)rc.left, (int)rc.top, (int)(rc.right - rc.left), (int)(rc.bottom - rc.top));
}

void UI::DrawRect(Gdiplus::Graphics* g, const RECT& rc, const Gdiplus::Color& color, float width) {
    if (!g) return;
    Gdiplus::Pen pen(color, width);
    g->DrawRectangle(&pen, (int)rc.left, (int)rc.top, (int)(rc.right - rc.left - 1), (int)(rc.bottom - rc.top - 1));
}

void UI::DrawRoundedRect(Gdiplus::Graphics* g, int x, int y, int w, int h, int radius,
                         const Gdiplus::Pen& pen, const Gdiplus::Brush* fill) {
    if (!g) return;
    Gdiplus::GraphicsPath path;
    int d = radius * 2;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    if (fill) g->FillPath(fill, &path);
    g->DrawPath(&pen, &path);
}

static void WidenRect(RECT& rc, int amount) {
    rc.left -= amount; rc.top -= amount;
    rc.right += amount; rc.bottom += amount;
}

void UI::DrawNeonBorder(Gdiplus::Graphics* g, int x, int y, int w, int h, const Gdiplus::Color& color) {
    if (!g) return;
    // Multi-pass glow: thick transparent → thin opaque
    for (int i = 4; i >= 1; i--) {
        int alpha = 25 + (5 - i) * 25;
        Gdiplus::Pen pen(Gdiplus::Color(alpha, color.GetR(), color.GetG(), color.GetB()), (float)i);
        g->DrawRectangle(&pen, x - i, y - i, w + i * 2, h + i * 2);
    }
    // Main border
    Gdiplus::Pen pen(color, 1.0f);
    g->DrawRectangle(&pen, x, y, w, h);
}

void UI::DrawSeparator(Gdiplus::Graphics* g, int y, int x1, int x2) {
    if (!g) return;
    Gdiplus::Pen pen(Theme::BORDER(), 1.0f);
    g->DrawLine(&pen, x1, y, x2, y);
}

void UI::DrawText(Gdiplus::Graphics* g, int x, int y, const char* text,
                  const Gdiplus::Color& color, const Gdiplus::Font* font) {
    if (!g || !text || !text[0]) return;
    if (!font) font = g_font;
    // Convert UTF-8 to wide
    int len = (int)strlen(text);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, len, nullptr, 0);
    std::wstring wstr(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, len, wstr.data(), wlen);

    Gdiplus::SolidBrush brush(color);
    Gdiplus::PointF pos((float)x, (float)y);
    g->DrawString(wstr.c_str(), wlen, font, pos, &brush);
}

void UI::DrawTextW(Gdiplus::Graphics* g, int x, int y, const wchar_t* text,
                   const Gdiplus::Color& color, const Gdiplus::Font* font) {
    if (!g || !text || !text[0]) return;
    if (!font) font = g_font;
    Gdiplus::SolidBrush brush(color);
    Gdiplus::PointF pos((float)x, (float)y);
    g->DrawString(text, -1, font, pos, &brush);
}

void UI::MeasureText(Gdiplus::Graphics* g, const char* text,
                     const Gdiplus::Font* font, int* w, int* h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!g) return;
    if (!font) font = g_font;
    if (!text || !text[0]) {
        return;
    }
    int len = (int)strlen(text);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, len, nullptr, 0);
    std::wstring wstr(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, len, wstr.data(), wlen);

    Gdiplus::RectF bounds;
    g->MeasureString(wstr.c_str(), wlen, font, Gdiplus::PointF(0, 0), &bounds);
    if (w) *w = (int)bounds.Width;
    if (h) *h = (int)bounds.Height;
}

// ============================================================
// Button
// ============================================================
bool UI::Button(UIContext& ctx, int id, const RECT& rc, const char* text) {
    bool hovered = ctx.PtInRect(rc);
    bool clicked = false;

    Gdiplus::Color bg, border, txt;
    if (hovered && ctx.mouseDown) {
        bg = Theme::BG_ACTIVE();
        border = Theme::ACCENT_LIGHT();
        txt = Theme::CLR_TEXT();
    } else if (hovered) {
        bg = Theme::BG_HOVER();
        border = Theme::ACCENT();
        txt = Theme::CLR_TEXT();
    } else {
        bg = Theme::BG_CONTROL();
        border = Theme::BORDER();
        txt = Theme::CLR_TEXT();
    }

    FillRect(ctx.g, rc, bg);
    DrawRect(ctx.g, rc, border);

    // Center text
    int tw, th;
    MeasureText(ctx.g, text, nullptr, &tw, &th);
    int tx = rc.left + (rc.right - rc.left - tw) / 2;
    int ty = rc.top + (rc.bottom - rc.top - th) / 2;
    DrawText(ctx.g, tx, ty, text, txt);

    if (hovered && ctx.mousePressed) {
        clicked = true;
    }

    return clicked;
}

// ============================================================
// Checkbox
// ============================================================
bool UI::Checkbox(UIContext& ctx, int id, const RECT& rc, const char* label, bool* value) {
    if (!ctx.g) return false;
    bool hovered = ctx.PtInRect(rc);
    bool changed = false;

    int boxSize = 14;
    RECT box = { rc.left, rc.top + (rc.bottom - rc.top - boxSize) / 2,
                 rc.left + boxSize, rc.top + (rc.bottom - rc.top + boxSize) / 2 };

    Gdiplus::Color bg = *value ? Theme::ACCENT() : Theme::BG_CONTROL();
    if (hovered) bg = *value ? Theme::ACCENT_LIGHT() : Theme::BG_HOVER();

    FillRect(ctx.g, box, bg);
    DrawRect(ctx.g, box, Theme::BORDER());

    // Checkmark
    if (*value) {
        Gdiplus::Pen pen(Theme::CLR_TEXT(), 2.0f);
        ctx.g->DrawLine(&pen, (int)box.left + 3, (int)box.top + 7, (int)box.left + 6, (int)box.top + 10);
        ctx.g->DrawLine(&pen, (int)box.left + 6, (int)box.top + 10, (int)box.left + 11, (int)box.top + 4);
    }

    // Label
    DrawText(ctx.g, box.right + 4, box.top, label, Theme::CLR_TEXT());

    if (hovered && ctx.mousePressed) {
        *value = !*value;
        changed = true;
    }
    return changed;
}

// ============================================================
// Text Input — with selection support (Ctrl+A/C/V, Shift+arrows)
// ============================================================
bool UI::TextInput(UIContext& ctx, int id, const RECT& rc, char* buf, int bufSize) {
    if (!ctx.g) return false;
    bool hovered = ctx.PtInRect(rc);
    bool focused = (ctx.focusId == id);
    bool changed = false;

    // Click to focus — clears any existing selection
    if (hovered && ctx.mousePressed) {
        ctx.focusId = id;
        ctx.caretPos = (int)strlen(buf);
        ctx.caretAnchor = ctx.caretPos;
        focused = true;
    }

    // Clamp caret positions to valid range
    int bufLen = (int)strlen(buf);
    if (ctx.caretPos < 0) ctx.caretPos = 0;
    if (ctx.caretPos > bufLen) ctx.caretPos = bufLen;
    if (ctx.caretAnchor < 0) ctx.caretAnchor = 0;
    if (ctx.caretAnchor > bufLen) ctx.caretAnchor = bufLen;

    // Compute selection range (used for both rendering and input handling)
    int selStart = std::min(ctx.caretPos, ctx.caretAnchor);
    int selEnd = std::max(ctx.caretPos, ctx.caretAnchor);
    bool hasSelection = (selStart != selEnd);

    // Background
    Gdiplus::Color bg = focused ? Theme::BG_PANEL() : Theme::BG_CONTROL();
    Gdiplus::Color border = focused ? Theme::ACCENT_LIGHT() : Theme::BORDER();
    FillRect(ctx.g, rc, bg);
    DrawRect(ctx.g, rc, border);

    // Clip to input rect so text doesn't overflow
    Gdiplus::Rect clipRect(rc.left + 1, rc.top + 1, rc.right - rc.left - 2, rc.bottom - rc.top - 2);
    Gdiplus::Region oldClip;
    ctx.g->GetClip(&oldClip);
    ctx.g->SetClip(clipRect);

    // Calculate text offset to keep caret visible
    int textX = rc.left + 4;
    int fullW = 0, fullH = 0;
    MeasureText(ctx.g, buf, nullptr, &fullW, &fullH);
    int boxW = rc.right - rc.left - 8;
    if (fullW > boxW) {
        // Text is wider than box — calculate offset to show caret
        int caretW = 0;
        if (ctx.caretPos > 0) {
            std::string beforeCaret(buf, ctx.caretPos);
            MeasureText(ctx.g, beforeCaret.c_str(), nullptr, &caretW, &fullH);
        }
        // If caret is past the right edge, scroll left
        if (caretW > boxW - 2) {
            textX = rc.left + 4 - (caretW - boxW + 4);
        }
    }

    // Selection highlight (drawn before text so text sits on top)
    if (focused && hasSelection) {
        int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        std::string beforeSel(buf, selStart);
        std::string selText(buf + selStart, selEnd - selStart);
        MeasureText(ctx.g, beforeSel.c_str(), nullptr, &w1, &h1);
        MeasureText(ctx.g, selText.c_str(), nullptr, &w2, &h2);
        RECT selRc = { textX + w1, rc.top, textX + w1 + w2, rc.bottom };
        FillRect(ctx.g, selRc, Theme::ACCENT());
    }

    // Draw text at adjusted position
    DrawText(ctx.g, textX, rc.top + 3, buf, Theme::CLR_TEXT());

    // Draw caret at adjusted position
    if (focused && ctx.caretBlink) {
        int beforeW = 0, th = 0;
        if (ctx.caretPos > 0) {
            std::string beforeCaret(buf, ctx.caretPos);
            MeasureText(ctx.g, beforeCaret.c_str(), nullptr, &beforeW, &th);
        }
        int caretX = textX + beforeW;
        // Clamp caret to visible area
        if (caretX < rc.left + 2) caretX = rc.left + 2;
        if (caretX > rc.right - 2) caretX = rc.right - 2;
        Gdiplus::Pen pen(Theme::CLR_TEXT(), 1.0f);
        ctx.g->DrawLine(&pen, caretX, rc.top + 2, caretX, rc.bottom - 2);
    }

    // Restore clip
    ctx.g->SetClip(&oldClip);

    // Handle input
    if (focused) {
        int len = (int)strlen(buf);

        // Character input — replaces selection if present
        if (ctx.hasCharInput && ctx.charInput >= 32 && ctx.charInput < 127) {
            if (hasSelection) {
                memmove(buf + selStart, buf + selEnd, len - selEnd + 1);
                ctx.caretPos = selStart;
                ctx.caretAnchor = selStart;
                len = (int)strlen(buf);
            }
            if (len < bufSize - 1) {
                memmove(buf + ctx.caretPos + 1, buf + ctx.caretPos, len - ctx.caretPos + 1);
                buf[ctx.caretPos] = (char)ctx.charInput;
                ctx.caretPos++;
                ctx.caretAnchor = ctx.caretPos;
                changed = true;
            }
        }

        if (ctx.keyPressed) {
            switch (ctx.keyCode) {
            case VK_BACK:
                if (hasSelection) {
                    memmove(buf + selStart, buf + selEnd, len - selEnd + 1);
                    ctx.caretPos = selStart;
                    ctx.caretAnchor = selStart;
                    changed = true;
                } else if (ctx.caretPos > 0) {
                    memmove(buf + ctx.caretPos - 1, buf + ctx.caretPos, len - ctx.caretPos + 1);
                    ctx.caretPos--;
                    ctx.caretAnchor = ctx.caretPos;
                    changed = true;
                }
                break;
            case VK_DELETE:
                if (hasSelection) {
                    memmove(buf + selStart, buf + selEnd, len - selEnd + 1);
                    ctx.caretPos = selStart;
                    ctx.caretAnchor = selStart;
                    changed = true;
                } else if (ctx.caretPos < len) {
                    memmove(buf + ctx.caretPos, buf + ctx.caretPos + 1, len - ctx.caretPos);
                    ctx.caretAnchor = ctx.caretPos;
                    changed = true;
                }
                break;
            case VK_HOME:
                ctx.caretPos = 0;
                if (!ctx.keyShift) ctx.caretAnchor = ctx.caretPos;
                break;
            case VK_END:
                ctx.caretPos = len;
                if (!ctx.keyShift) ctx.caretAnchor = ctx.caretPos;
                break;
            case VK_LEFT:
                if (ctx.caretPos > 0) ctx.caretPos--;
                if (!ctx.keyShift) ctx.caretAnchor = ctx.caretPos;
                break;
            case VK_RIGHT:
                if (ctx.caretPos < len) ctx.caretPos++;
                if (!ctx.keyShift) ctx.caretAnchor = ctx.caretPos;
                break;
            case 'A':
                if (ctx.keyCtrl) { ctx.caretPos = 0; ctx.caretAnchor = len; }
                break;
            case 'C':
                if (ctx.keyCtrl) {
                    if (hasSelection) {
                        std::string sel(buf + selStart, selEnd - selStart);
                        CopyToClipboard(ctx.hwnd, sel.c_str());
                    } else {
                        CopyToClipboard(ctx.hwnd, buf);
                    }
                }
                break;
            case 'V':
                if (ctx.keyCtrl) {
                    std::string clip = PasteFromClipboard(ctx.hwnd);
                    if (!clip.empty()) {
                        if (hasSelection) {
                            // Delete selection first
                            memmove(buf + selStart, buf + selEnd, len - selEnd + 1);
                            ctx.caretPos = selStart;
                            len = (int)strlen(buf);
                        }
                        int clipLen = (int)clip.size();
                        int remain = bufSize - 1 - len;
                        int toPaste = std::min(clipLen, remain);
                        memmove(buf + ctx.caretPos + toPaste, buf + ctx.caretPos, len - ctx.caretPos + 1);
                        memcpy(buf + ctx.caretPos, clip.c_str(), toPaste);
                        ctx.caretPos += toPaste;
                        ctx.caretAnchor = ctx.caretPos;
                        changed = true;
                    }
                }
                break;
            }
        }
    }

    return changed;
}

// ============================================================
// Combo Box (native popup menu)
// ============================================================
bool UI::ComboBox(UIContext& ctx, int id, const RECT& rc, const char** items, int count, int* selected) {
    if (!ctx.g) return false;
    bool hovered = ctx.PtInRect(rc);
    bool changed = false;

    Gdiplus::Color bg = hovered ? Theme::BG_HOVER() : Theme::BG_CONTROL();
    Gdiplus::Color border = hovered ? Theme::ACCENT() : Theme::BORDER();
    FillRect(ctx.g, rc, bg);
    DrawRect(ctx.g, rc, border);

    // Selected text
    const char* text = (*selected >= 0 && *selected < count) ? items[*selected] : "";
    DrawText(ctx.g, rc.left + 4, rc.top + 3, text, Theme::CLR_TEXT());

    // Dropdown arrow
    int ax = rc.right - 14;
    int ay = rc.top + (rc.bottom - rc.top) / 2;
    Gdiplus::Pen pen(Theme::CLR_DIM(), 1.0f);
    ctx.g->DrawLine(&pen, ax - 3, ay - 2, ax, ay + 2);
    ctx.g->DrawLine(&pen, ax, ay + 2, ax + 3, ay - 2);

    if (hovered && ctx.mousePressed) {
        // Defer the popup menu — can't call TrackPopupMenu during WM_PAINT
        // Copy items array since caller's stack frame will be gone
        ctx.pendingComboId = id;
        ctx.pendingComboRc = rc;
        ctx.pendingComboCount = std::min(count, UIContext::MAX_COMBO_ITEMS);
        for (int i = 0; i < ctx.pendingComboCount; i++) {
            ctx.pendingComboItems[i] = items[i];  // copies the const char* (string literals are static, safe)
        }
        ctx.pendingComboSelected = *selected;  // copy current value
        ctx.pendingComboSelectedPtr = selected;  // remember where to write back
    }

    return changed;
}

// ============================================================
// Progress Bar
// ============================================================
void UI::ProgressBar(UIContext& ctx, const RECT& rc, float progress) {
    FillRect(ctx.g, rc, Theme::BG_CONTROL());
    DrawRect(ctx.g, rc, Theme::BORDER());

    if (progress > 0) {
        int fillW = (int)((rc.right - rc.left - 2) * progress);
        RECT fill = { rc.left + 1, rc.top + 1, rc.left + 1 + fillW, rc.bottom - 1 };
        FillRect(ctx.g, fill, Theme::ACCENT());
    }

    // Text
    char txt[32];
    snprintf(txt, sizeof(txt), "%.0f%%", progress * 100.0f);
    int tw, th;
    MeasureText(ctx.g, txt, nullptr, &tw, &th);
    DrawText(ctx.g, rc.left + (rc.right - rc.left - tw) / 2,
             rc.top + (rc.bottom - rc.top - th) / 2, txt, Theme::CLR_TEXT());
}

// ============================================================
// Label
// ============================================================
void UI::Label(UIContext& ctx, int x, int y, const char* text, const Gdiplus::Color& color) {
    DrawText(ctx.g, x, y, text, color);
}

// ============================================================
// Scrollbar
// ============================================================
bool UI::Scrollbar(UIContext& ctx, int id, const RECT& rc, int thumbH, int total, int visible, int* scrollPos) {
    if (total <= visible) return false;

    int maxScroll = total - visible;
    if (maxScroll <= 0) return false;
    int trackH = rc.bottom - rc.top - thumbH;
    if (trackH <= 0) return false;

    bool changed = false;

    // Compute thumb Y
    int thumbY = rc.top + trackH * (*scrollPos) / maxScroll;

    // Check if mouse is on thumb
    RECT thumbRc = { rc.left, thumbY, rc.right, thumbY + thumbH };
    bool onThumb = ctx.PtInRect(thumbRc);
    bool onTrack = ctx.PtInRect(rc);

    // Start drag
    if (onThumb && ctx.mousePressed) {
        ctx.scrollDragId = id;
        ctx.scrollDragOffset = ctx.mouse.y - thumbY;
    }

    // End drag
    if (!ctx.mouseDown && ctx.scrollDragId == id) {
        ctx.scrollDragId = -1;
    }

    // During drag
    if (ctx.scrollDragId == id) {
        int newThumbY = ctx.mouse.y - ctx.scrollDragOffset;
        newThumbY = std::max<int>(rc.top, std::min<int>(newThumbY, rc.top + trackH));
        int newScroll = (newThumbY - rc.top) * maxScroll / trackH;
        if (newScroll != *scrollPos) {
            *scrollPos = newScroll;
            changed = true;
        }
    }

    // Click on track (page up/down)
    if (onTrack && !onThumb && ctx.mousePressed) {
        if (ctx.mouse.y < thumbY) {
            *scrollPos = std::max(0, *scrollPos - visible);
        } else {
            *scrollPos = std::min(maxScroll, *scrollPos + visible);
        }
        changed = true;
    }

    // Recompute thumb position after scroll changes so draw reflects current state
    thumbY = rc.top + trackH * (*scrollPos) / maxScroll;
    thumbRc = { rc.left, thumbY, rc.right, thumbY + thumbH };

    // Draw
    Gdiplus::Color trackColor = Theme::BG_TITLE();
    Gdiplus::Color thumbColor = (onThumb || ctx.scrollDragId == id) ? Theme::ACCENT_LIGHT() : Theme::BG_HOVER();
    FillRect(ctx.g, rc, trackColor);
    FillRect(ctx.g, thumbRc, thumbColor);

    return changed;
}
