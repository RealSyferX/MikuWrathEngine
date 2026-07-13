#include "app.h"
#include "ui.h"
#include "privilege_utils.h"
#include <gdiplus.h>
#include <dwmapi.h>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

static HWND g_hwnd = nullptr;
static UINT_PTR g_caretTimer = 0;
static App* g_app = nullptr;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        // Guard against re-entrant paints.  A modal loop (TrackPopupMenu,
        // GetSaveFileName, etc.) running inside ProcessPendingActions can
        // dispatch a WM_PAINT while we are still inside OnPaint.  Skip the
        // nested paint entirely — the outer paint will finish and the
        // InvalidateRect calls will queue a fresh one.
        static bool s_inPaint = false;
        if (s_inPaint || !g_app) {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            EndPaint(hWnd, &ps);
            return 0;
        }
        s_inPaint = true;

        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        // Zero-size window (minimised, just-created, etc.) — nothing to draw.
        // CreateCompatibleBitmap(hdc, 0, 0) returns NULL which would crash
        // the Graphics constructor or BitBlt below.
        if (w <= 0 || h <= 0) {
            EndPaint(hWnd, &ps);
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
                g_app->m_ui.g = &g;
                g_app->m_ui.width = w;
                g_app->m_ui.height = h;
                g_app->OnPaint(&g);
            }
            // Graphics object is now destroyed — clear the dangling pointer
            // so that no handler (WM_TIMER, WM_KEYDOWN, …) can use it
            // before the next WM_PAINT sets it again.
            g_app->m_ui.g = nullptr;

            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
        }
        if (memDC) DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        s_inPaint = false;
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (!g_app) break;
        g_app->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false, false);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (!g_app) break;
        g_app->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false, true);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_RBUTTONDOWN:
        if (!g_app) break;
        g_app->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true, false);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (!g_app) break;
        g_app->OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (!g_app) break;
        g_app->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        if (!g_app) break;
        g_app->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (!g_app) break;
        g_app->OnKeyDown(wParam);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_CHAR:
        if (!g_app) break;
        g_app->OnChar((wchar_t)wParam);
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (!g_app) break;
        g_app->OnTimer();
        return 0;
    case WM_NCHITTEST: {
        POINT pos;
        pos.x = (int)(short)LOWORD(lParam);
        pos.y = (int)(short)HIWORD(lParam);
        ScreenToClient(hWnd, &pos);
        RECT rcc; GetClientRect(hWnd, &rcc);
        const int border = 6;
        bool left = pos.x <= border, right = pos.x >= rcc.right - border;
        bool top = pos.y <= border, bottom = pos.y >= rcc.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
        mmi->ptMinTrackSize.x = 600;
        mmi->ptMinTrackSize.y = 400;
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Enable SE_DEBUG_NAME so OpenProcess/DebugActiveProcess can target
    // elevated or protected processes.  Fails gracefully when not elevated.
    EnableDebugPrivilege();

    // GDI+ startup
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartupInput gdiplusStartup;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartup, nullptr);

    // Register window class
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.style = CS_CLASSDC | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MikuWrathEngine";

    RegisterClassExW(&wc);

    // Create borderless overlay window
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        wc.lpszClassName, L"MikuWrathEngine",
        WS_POPUP,
        120, 120, 960, 640,
        nullptr, nullptr, hInstance, nullptr);

    g_hwnd = hwnd;

    // Rounded corners (Win11)
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
    int cornerPref = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // Shadow
    MARGINS margins = { 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Init fonts
    UI::InitFonts();

    // Create app
    App app;
    app.SetHwnd(hwnd);
    g_app = &app;

    // Caret timer
    g_caretTimer = SetTimer(hwnd, 1, 500, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);

        // Handle pending drag
        if (app.TakePendingDrag()) {
            POINT pos;
            GetCursorPos(&pos);
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pos.x, pos.y));
        }

        app.ProcessPendingActions();
    }

    // Cleanup
    KillTimer(hwnd, g_caretTimer);
    UI::CleanupFonts();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}
