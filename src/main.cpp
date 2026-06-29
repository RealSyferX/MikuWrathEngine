#include "app.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <cstdio>

#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static UINT                    g_ResizeWidth = 0;
static UINT                    g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND                    g_hwnd = nullptr;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#ifdef DEBUG_BUILD
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL flSrc[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_1 };
    D3D_FEATURE_LEVEL flOut;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, flSrc, ARRAYSIZE(flSrc),
        D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &flOut, &g_pd3dDeviceContext);
    if (hr != S_OK) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_NCHITTEST: {
        POINT pos;
        pos.x = (int)(short)LOWORD(lParam);
        pos.y = (int)(short)HIWORD(lParam);
        ScreenToClient(hWnd, &pos);

        RECT rc;
        GetClientRect(hWnd, &rc);

        const int border = 6;

        bool left   = pos.x <= border;
        bool right  = pos.x >= rc.right - border;
        bool top    = pos.y <= border;
        bool bottom = pos.y >= rc.bottom - border;

        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (top)             return HTTOP;
        if (bottom)          return HTBOTTOM;
        if (left)            return HTLEFT;
        if (right)           return HTRIGHT;

        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
        mmi->ptMinTrackSize.x = 480;
        mmi->ptMinTrackSize.y = 320;
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L,
                       hInstance, nullptr, nullptr, nullptr, nullptr,
                       L"MikuWrathEngine", nullptr };
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Borderless overlay: WS_POPUP + always-on-top
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        wc.lpszClassName, L"MikuWrathEngine",
        WS_POPUP,
        120, 120, 960, 640,
        nullptr, nullptr, hInstance, nullptr);

    g_hwnd = hwnd;

    // Rounded corners (Windows 11)
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
    int cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // Drop shadow for borderless window
    MARGINS margins = { 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Failed to initialize DirectX 11.", L"Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load Consolas font (monospace, good for hex/disasm)
    ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\consola.ttf", 15.0f, nullptr,
        io.Fonts->GetGlyphRangesDefault());
    if (font) {
        io.FontDefault = font;
    }

    // Dark theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.PopupRounding = 2.0f;

    // Custom colors (dark blue-ish overlay)
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]        = ImVec4(0.06f, 0.06f, 0.08f, 1.0f);
    colors[ImGuiCol_ChildBg]         = ImVec4(0.04f, 0.04f, 0.06f, 1.0f);
    colors[ImGuiCol_FrameBg]         = ImVec4(0.12f, 0.12f, 0.16f, 1.0f);
    colors[ImGuiCol_FrameBgHovered]  = ImVec4(0.20f, 0.20f, 0.28f, 1.0f);
    colors[ImGuiCol_FrameBgActive]   = ImVec4(0.24f, 0.24f, 0.32f, 1.0f);
    colors[ImGuiCol_Header]          = ImVec4(0.20f, 0.22f, 0.70f, 0.5f);
    colors[ImGuiCol_HeaderHovered]   = ImVec4(0.30f, 0.32f, 0.80f, 0.5f);
    colors[ImGuiCol_HeaderActive]    = ImVec4(0.40f, 0.42f, 0.90f, 0.5f);
    colors[ImGuiCol_Button]          = ImVec4(0.20f, 0.22f, 0.32f, 1.0f);
    colors[ImGuiCol_ButtonHovered]   = ImVec4(0.28f, 0.30f, 0.50f, 1.0f);
    colors[ImGuiCol_ButtonActive]    = ImVec4(0.35f, 0.38f, 0.60f, 1.0f);
    colors[ImGuiCol_CheckMark]       = ImVec4(0.40f, 0.70f, 1.0f, 1.0f);
    colors[ImGuiCol_Separator]       = ImVec4(0.15f, 0.15f, 0.18f, 1.0f);
    colors[ImGuiCol_Border]          = ImVec4(0.15f, 0.15f, 0.20f, 1.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    App app;
    app.SetHwnd(hwnd);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Handle resize
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = 0;
            g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.Render();

        ImGui::Render();

        const float clearColor[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);

        // Handle pending window drag (after present so frame is visible)
        if (app.TakePendingDrag()) {
            POINT pos;
            GetCursorPos(&pos);
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pos.x, pos.y));
        }
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
