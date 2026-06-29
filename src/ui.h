#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <string>

#pragma comment(lib, "gdiplus.lib")

// ============================================================
// VS Blue Dark Theme + Neon Glow
// ============================================================
namespace Theme {
    using Gdiplus::Color;

    // Backgrounds
    inline Color BG_MAIN()      { return Color(28, 28, 42); }       // #1C1C2A
    inline Color BG_PANEL()     { return Color(34, 34, 48); }       // #222230
    inline Color BG_CONTROL()   { return Color(42, 42, 58); }       // #2A2A3A
    inline Color BG_HOVER()     { return Color(52, 52, 72); }       // #343448
    inline Color BG_ACTIVE()    { return Color(62, 62, 88); }       // #3E3E58
    inline Color BG_SELECTED()  { return Color(38, 64, 120); }      // #264078
    inline Color BG_TITLE()     { return Color(22, 22, 36); }       // #161624

    // Borders
    inline Color BORDER()       { return Color(48, 48, 64); }       // #303040
    inline Color BORDER_HOT()   { return Color(0, 122, 204); }     // #007ACC

    // Neon
    inline Color NEON()         { return Color(0, 168, 255); }     // #00A8FF
    inline Color NEON_GLOW(int a) { return Color(a, 0, 168, 255); }

    // Text (avoid TEXT macro conflict from windows.h)
    inline Color CLR_TEXT()     { return Color(220, 220, 236); }
    inline Color CLR_DIM()      { return Color(120, 120, 144); }
    inline Color CLR_GREEN()    { return Color(78, 201, 176); }
    inline Color CLR_RED()      { return Color(244, 71, 71); }
    inline Color CLR_BLUE()     { return Color(86, 156, 214); }
    inline Color CLR_YELLOW()   { return Color(220, 220, 120); }

    // Accent
    inline Color ACCENT()       { return Color(0, 122, 204); }     // #007ACC
    inline Color ACCENT_LIGHT() { return Color(0, 168, 255); }     // #00A8FF

    // Sizes
    constexpr int TITLE_H = 30;
    constexpr int MENU_H = 22;
    constexpr int ROW_H = 18;
    constexpr int BTN_H = 24;
    constexpr int PAD = 6;
}

// ============================================================
// UI Context — tracks input state for immediate-mode widgets
// ============================================================
struct UIContext {
    // Mouse
    POINT mouse = {0, 0};
    bool mouseDown = false;
    bool mousePressed = false;
    bool mouseReleased = false;
    bool mouseDoubleClicked = false;
    bool mouseRightPressed = false;
    int mouseWheel = 0;

    // Keyboard
    wchar_t charInput = 0;
    bool hasCharInput = false;
    int keyCode = 0;
    bool keyPressed = false;
    bool keyCtrl = false;
    bool keyShift = false;

    // Focus (for text inputs)
    int focusId = -1;
    int caretPos = 0;
    int caretAnchor = 0;
    bool caretBlink = true;

    // Rendering
    Gdiplus::Graphics* g = nullptr;
    HWND hwnd = nullptr;
    int width = 0, height = 0;

    // Pending combo (deferred from paint to avoid modal loop during WM_PAINT)
    int pendingComboId = -1;
    RECT pendingComboRc = {};
    const char** pendingComboItems = nullptr;
    int pendingComboCount = 0;
    int* pendingComboSelected = nullptr;

    // Scrollbar drag state
    int scrollDragId = -1;
    int scrollDragOffset = 0;

    bool PtInRect(const RECT& rc) const {
        return mouse.x >= rc.left && mouse.x < rc.right &&
               mouse.y >= rc.top && mouse.y < rc.bottom;
    }
};

// ============================================================
// Widget declarations (immediate-mode, GDI+ rendered)
// ============================================================
namespace UI {

// Primitives
void FillRect(Gdiplus::Graphics* g, const RECT& rc, const Gdiplus::Color& color);
void DrawRect(Gdiplus::Graphics* g, const RECT& rc, const Gdiplus::Color& color, float width = 1.0f);
void DrawRoundedRect(Gdiplus::Graphics* g, int x, int y, int w, int h, int radius,
                     const Gdiplus::Pen& pen, const Gdiplus::Brush* fill = nullptr);
void DrawText(Gdiplus::Graphics* g, int x, int y, const char* text,
              const Gdiplus::Color& color, const Gdiplus::Font* font = nullptr);
void DrawTextW(Gdiplus::Graphics* g, int x, int y, const wchar_t* text,
               const Gdiplus::Color& color, const Gdiplus::Font* font = nullptr);
void MeasureText(Gdiplus::Graphics* g, const char* text,
                 const Gdiplus::Font* font, int* w, int* h);
void DrawNeonBorder(Gdiplus::Graphics* g, int x, int y, int w, int h, const Gdiplus::Color& color);
void DrawSeparator(Gdiplus::Graphics* g, int y, int x1, int x2);

// Widgets
bool Button(UIContext& ctx, int id, const RECT& rc, const char* text);
bool Checkbox(UIContext& ctx, int id, const RECT& rc, const char* label, bool* value);
bool TextInput(UIContext& ctx, int id, const RECT& rc, char* buf, int bufSize);
bool ComboBox(UIContext& ctx, int id, const RECT& rc, const char** items, int count, int* selected);
void ProgressBar(UIContext& ctx, const RECT& rc, float progress);
void Label(UIContext& ctx, int x, int y, const char* text, const Gdiplus::Color& color);
bool Scrollbar(UIContext& ctx, int id, const RECT& rc, int thumbH, int total, int visible, int* scrollPos);

// Fonts
void InitFonts();
void CleanupFonts();
extern Gdiplus::Font* g_font;
extern Gdiplus::Font* g_fontBold;
extern Gdiplus::Font* g_fontSmall;

} // namespace UI
