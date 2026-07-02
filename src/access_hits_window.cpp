#include "access_hits_window.h"
#include <windowsx.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

static bool s_inPaint = false;

LRESULT CALLBACK AccessHitsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AccessHitsWindow* self = nullptr;
    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<AccessHitsWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<AccessHitsWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

AccessHitsWindow::AccessHitsWindow() {}
AccessHitsWindow::~AccessHitsWindow() {
    if (m_caretTimer) KillTimer(m_hwnd, m_caretTimer);
    if (m_hwnd) DestroyWindow(m_hwnd);
}

void AccessHitsWindow::Create(HWND parent, HINSTANCE hInst) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.style = CS_CLASSDC | CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = ClassName();
        RegisterClassExW(&wc);
        registered = true;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        ClassName(), L"Find what accesses",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 650, 350,
        parent, nullptr, hInst, this);

    m_ui.hwnd = m_hwnd;
    m_caretTimer = SetTimer(m_hwnd, 1, 500, nullptr);
}

void AccessHitsWindow::Show() {
    if (m_hwnd) ShowWindow(m_hwnd, SW_SHOW);
}

void AccessHitsWindow::Hide() {
    if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
    // Drop cached disassembly so a fresh find repopulates from the live
    // process image rather than showing stale text.
    m_disasmCache.clear();
}

void AccessHitsWindow::SetTitle(const char* title) {
    strncpy(m_title, title, sizeof(m_title) - 1);
    m_title[sizeof(m_title) - 1] = '\0';
    if (m_hwnd) {
        wchar_t wtitle[128];
        MultiByteToWideChar(CP_UTF8, 0, m_title, -1, wtitle, 128);
        SetWindowTextW(m_hwnd, wtitle);
    }
}

void AccessHitsWindow::ProcessPendingActions() {
    // Combo boxes if any (none for now)
}

LRESULT AccessHitsWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        if (s_inPaint) {
            PAINTSTRUCT ps;
            BeginPaint(m_hwnd, &ps);
            EndPaint(m_hwnd, &ps);
            return 0;
        }
        s_inPaint = true;

        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hwnd, &ps);
        RECT rc; GetClientRect(m_hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        if (w <= 0 || h <= 0) {
            EndPaint(m_hwnd, &ps);
            s_inPaint = false;
            return 0;
        }

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBmp = memBmp ? (HBITMAP)SelectObject(memDC, memBmp) : nullptr;

        if (memDC && memBmp) {
            {
                Gdiplus::Graphics g(memDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                OnPaint(&g, w, h);
            }
            m_ui.g = nullptr;
            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
        }
        if (memDC) DeleteDC(memDC);
        EndPaint(m_hwnd, &ps);
        s_inPaint = false;
        return 0;
    }
    case WM_LBUTTONDOWN:
        OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false, false);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        m_ui.mouseDown = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_RBUTTONDOWN:
        OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true, false);
        return 0;
    case WM_LBUTTONDBLCLK:
        OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false, true);
        return 0;
    case WM_MOUSEMOVE:
        m_ui.mouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_TIMER:
        m_caretBlink = !m_caretBlink;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        Hide();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETMINMAXINFO: {
        LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
        mmi->ptMinTrackSize.x = 400;
        mmi->ptMinTrackSize.y = 250;
        return 0;
    }
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

void AccessHitsWindow::OnMouseDown(int x, int y, bool right, bool dbl) {
    m_ui.mouse = { x, y };
    m_ui.mousePressed = !right && !dbl;
    m_ui.mouseDown = !right;
    m_ui.mouseDoubleClicked = dbl;
    m_ui.mouseRightPressed = right;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AccessHitsWindow::OnMouseWheel(int delta) {
    m_scrollPos -= delta / 120 * 3;
    m_scrollPos = std::max(0, m_scrollPos);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AccessHitsWindow::OnPaint(Gdiplus::Graphics* g, int w, int h) {
    m_ui.g = g;
    m_ui.width = w;
    m_ui.height = h;

    // Background
    UI::FillRect(g, {0, 0, w, h}, Theme::BG_MAIN());
    UI::DrawNeonBorder(g, 0, 0, w - 1, h - 1, Theme::NEON());

    // Title bar
    UI::FillRect(g, {0, 0, w, 30}, Theme::BG_TITLE());
    UI::DrawText(g, 8, 7, m_title, Theme::CLR_TEXT());

    // Stop button
    RECT stopBtn = {w - 80, 3, w - 10, 27};
    if (UI::Button(m_ui, 9000, stopBtn, "Stop")) {
        if (m_debugger) m_debugger->StopFind();
        Hide();
    }

    // Status
    bool finding = m_debugger && m_debugger->IsFinding();
    UI::DrawText(g, w / 2 - 50, 7, finding ? "[ Monitoring ]" : "[ Stopped ]",
                 finding ? Theme::CLR_GREEN() : Theme::CLR_DIM());

    UI::DrawSeparator(g, 30, 2, w - 2);

    // Get hits
    std::vector<AccessHit> hits;
    if (m_debugger) hits = m_debugger->GetAccessHits();

    int rowH = std::max(16, UI::g_fontSize + 7);
    int yStart = 35;
    int visibleRows = std::max(1, (h - yStart - 25) / rowH);

    int maxScroll = std::max(0, (int)hits.size() - visibleRows);
    m_scrollPos = std::min(m_scrollPos, maxScroll);
    m_scrollPos = std::max(0, m_scrollPos);

    // Headers
    UI::FillRect(g, {0, yStart, w, yStart + rowH}, Theme::BG_CONTROL());
    UI::DrawText(g, 8, yStart + 2, "Instruction", Theme::CLR_DIM());
    UI::DrawText(g, 180, yStart + 2, "Disassembly", Theme::CLR_DIM());
    UI::DrawText(g, 390, yStart + 2, "Count", Theme::CLR_DIM());
    UI::DrawText(g, 450, yStart + 2, "Thread", Theme::CLR_DIM());
    if (w > 400) UI::DrawText(g, 530, yStart + 2, "Module+offset", Theme::CLR_DIM());
    yStart += rowH + 2;

    for (int i = 0; i < visibleRows; i++) {
        int idx = m_scrollPos + i;
        if (idx >= (int)hits.size()) break;

        int y = yStart + i * rowH;
        Gdiplus::Color rowBg = i % 2 ? Theme::BG_MAIN() : Theme::BG_PANEL();
        UI::FillRect(g, {0, y, w, y + rowH}, rowBg);

        char buf[32];
        snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)hits[idx].instruction);
        UI::DrawText(g, 8, y + 1, buf, Theme::CLR_BLUE());

        // Disassembly text (cached per instruction address). Reading 16 bytes
        // gives capstone enough context for any single x86-64 instruction.
        auto cacheIt = m_disasmCache.find(hits[idx].instruction);
        if (cacheIt == m_disasmCache.end()) {
            char disasmStr[256] = {};
            if (m_dis && m_dis->IsInitialized() && m_pm && m_pm->IsOpen()) {
                uint8_t dbuf[16];
                if (m_pm->Read(hits[idx].instruction, dbuf, sizeof(dbuf))) {
                    auto insts = m_dis->Disassemble(hits[idx].instruction, dbuf, sizeof(dbuf), 1);
                    if (!insts.empty()) {
                        snprintf(disasmStr, sizeof(disasmStr), "%s %s",
                                 insts[0].mnemonic, insts[0].opStr);
                    }
                }
            }
            if (!disasmStr[0]) strcpy(disasmStr, "??");
            m_disasmCache[hits[idx].instruction] = disasmStr;
            cacheIt = m_disasmCache.find(hits[idx].instruction);
        }
        UI::DrawText(g, 180, y + 1, cacheIt->second.c_str(), Theme::CLR_TEXT());

        snprintf(buf, sizeof(buf), "%d", hits[idx].count);
        UI::DrawText(g, 390, y + 1, buf, Theme::CLR_YELLOW());

        snprintf(buf, sizeof(buf), "%lu", hits[idx].threadId);
        UI::DrawText(g, 450, y + 1, buf, Theme::CLR_TEXT());

        // Module-relative address
        if (w > 400 && m_pm && m_pm->IsOpen()) {
            std::string modAddr = m_pm->FormatAddress(hits[idx].instruction);
            UI::DrawText(g, 530, y + 1, modAddr.c_str(), Theme::CLR_DIM());
        }

        // Double-click to copy address to clipboard + jump to it
        RECT rowRc = {0, y, w, y + rowH};
        if (m_ui.PtInRect(rowRc) && m_ui.mouseDoubleClicked) {
            char clip[32];
            snprintf(clip, sizeof(clip), "0x%llX", (unsigned long long)hits[idx].instruction);
            CopyToClipboard(m_hwnd, clip);
            if (m_goToCallback) m_goToCallback(hits[idx].instruction);
        }
    }

    if (hits.empty()) {
        UI::DrawText(g, 8, yStart + 4, "Waiting for hits...", Theme::CLR_DIM());
    }

    // Scrollbar
    if (hits.size() > (size_t)visibleRows) {
        int sbW = 12;
        int sbX = w - sbW;
        int sbH = h - yStart - 5;
        int thumbH = std::max(20, sbH * visibleRows / (int)hits.size());
        UI::Scrollbar(m_ui, 9100, {sbX, yStart, w, h - 5}, thumbH, (int)hits.size(), visibleRows, &m_scrollPos);
    }

    // Footer
    char info[64];
    snprintf(info, sizeof(info), "Total: %zu hits | %s", hits.size(), finding ? "Monitoring..." : "Stopped");
    UI::DrawText(g, 8, h - 18, info, Theme::CLR_DIM());

    // Reset input flags
    m_ui.mousePressed = false;
    m_ui.mouseReleased = false;
    m_ui.mouseDoubleClicked = false;
    m_ui.mouseRightPressed = false;
    m_ui.hasCharInput = false;
    m_ui.keyPressed = false;
    m_ui.mouseWheel = 0;
}
