#include "memory_viewer.h"
#include <windowsx.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>

// ============================================================
// Window class registration + WndProc
// ============================================================
LRESULT CALLBACK MemoryViewer::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MemoryViewer* self = nullptr;
    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<MemoryViewer*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<MemoryViewer*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

MemoryViewer::MemoryViewer() {}

MemoryViewer::~MemoryViewer() {
    if (m_caretTimer) KillTimer(m_hwnd, m_caretTimer);
    if (m_hwnd) DestroyWindow(m_hwnd);
}

void MemoryViewer::Create(HWND parent, HINSTANCE hInst) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = ClassName();
        RegisterClassExW(&wc);
        registered = true;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        ClassName(), L"Memory Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 500,
        parent, nullptr, hInst, this);

    m_ui.hwnd = m_hwnd;
    m_caretTimer = SetTimer(m_hwnd, 1, 500, nullptr);
}

void MemoryViewer::Show() {
    if (m_hwnd) ShowWindow(m_hwnd, SW_SHOW);
}

void MemoryViewer::Hide() {
    if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
}

LRESULT MemoryViewer::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hwnd, &ps);
        RECT rc; GetClientRect(m_hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;

        // Double buffer
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        {
            Gdiplus::Graphics g(memDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            OnPaint(&g, w, h);
        }

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(m_hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false, false);
        return 0;
    case WM_LBUTTONDBLCLK:
        OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false, true);
        return 0;
    case WM_RBUTTONDOWN:
        OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true, false);
        return 0;
    case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_CHAR:
        OnChar((wchar_t)wParam);
        return 0;
    case WM_KEYDOWN:
        OnKeyDown((WPARAM)wParam);
        return 0;
    case WM_TIMER:
        m_caretBlink = !m_caretBlink;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        ShowWindow(m_hwnd, SW_HIDE);
        return 0;
    case WM_ERASEBKGND:
        return 1; // prevent flicker
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

// ============================================================
// Core logic
// ============================================================
void MemoryViewer::GoToAddress(uintptr_t addr) {
    m_hexAddr = addr;
    m_disasmAddr = addr;
    snprintf(m_addrBuf, sizeof(m_addrBuf), "%llX", (unsigned long long)addr);
    RefreshDisasm();
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MemoryViewer::ParseAndGo() {
    char* end = nullptr;
    unsigned long long val = strtoull(m_addrBuf, &end, 16);
    if (end != m_addrBuf) {
        m_hexAddr = (uintptr_t)val;
        m_disasmAddr = (uintptr_t)val;
        RefreshDisasm();
    }
}

void MemoryViewer::RefreshDisasm() {
    m_disasmView.clear();
    if (!m_pm || !m_pm->IsOpen() || !m_dis || !m_dis->IsInitialized()) return;
    uint8_t buf[512];
    if (m_pm->Read(m_disasmAddr, buf, sizeof(buf))) {
        m_disasmView = m_dis->Disassemble(m_disasmAddr, buf, sizeof(buf), 32);
    }
}

void MemoryViewer::ScrollHex(int lines) {
    uintptr_t delta = (uintptr_t)(lines * m_hexCols);
    if (lines < 0) {
        uintptr_t newAddr = m_hexAddr - delta;
        if (newAddr > m_hexAddr) return;
        m_hexAddr = newAddr;
    } else {
        m_hexAddr += delta;
    }
}

void MemoryViewer::ScrollDisasm(int lines) {
    if (lines > 0) {
        if (m_disasmView.size() > 1) {
            m_disasmAddr = m_disasmView[1].address;
            m_disasmView.erase(m_disasmView.begin());
            if (m_disasmView.size() < 8 && !m_disasmView.empty()) {
                uintptr_t nextAddr = m_disasmView.back().address + m_disasmView.back().size;
                uint8_t buf[256];
                if (m_pm->Read(nextAddr, buf, sizeof(buf))) {
                    auto more = m_dis->Disassemble(nextAddr, buf, sizeof(buf), 24);
                    m_disasmView.insert(m_disasmView.end(), more.begin(), more.end());
                }
            }
        }
    } else if (lines < 0) {
        uintptr_t prevAddr = m_dis->FindPreviousInstruction(m_disasmAddr, m_pm);
        if (prevAddr != m_disasmAddr) {
            m_disasmAddr = prevAddr;
            RefreshDisasm();
        }
    }
}

void MemoryViewer::DoNOP(uintptr_t addr, size_t len) {
    if (!m_pm || !m_pm->IsOpen() || len == 0) return;
    std::vector<uint8_t> nops(len, 0x90);
    m_pm->Write(addr, nops.data(), len);
    RefreshDisasm();
}

void MemoryViewer::DoPatch(uintptr_t addr, const char* hexStr) {
    if (!m_pm || !m_pm->IsOpen()) return;
    std::vector<uint8_t> bytes;
    std::istringstream iss(hexStr);
    std::string tok;
    while (iss >> tok) {
        bytes.push_back((uint8_t)strtoul(tok.c_str(), nullptr, 16));
    }
    if (!bytes.empty()) {
        m_pm->Write(addr, bytes.data(), bytes.size());
        RefreshDisasm();
    }
}

void MemoryViewer::ShowContextMenu(int x, int y, uintptr_t addr, bool isDisasm, size_t instSize) {
    HMENU menu = CreatePopupMenu();
    if (m_addToTable) {
        AppendMenuA(menu, MF_STRING, 1, "Add to Address Table");
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    }
    if (isDisasm) {
        AppendMenuA(menu, MF_STRING, 2, "NOP This Instruction");
        AppendMenuA(menu, MF_STRING, 3, "Patch Bytes...");
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    } else {
        AppendMenuA(menu, MF_STRING, 4, "Edit Bytes...");
        AppendMenuA(menu, MF_STRING, 5, "NOP Line");
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuA(menu, MF_STRING, 6, "Follow in Hex View");
    if (isDisasm) AppendMenuA(menu, MF_STRING, 7, "Go to Disasm");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, 8, "Copy Address");

    POINT pt = { x, y };
    ClientToScreen(m_hwnd, &pt);
    int result = (int)TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
        pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    switch (result) {
    case 1: if (m_addToTable) m_addToTable(addr, ValueType::Dword); break;
    case 2: DoNOP(addr, instSize); break;
    case 3: m_patchAddr = addr; m_patchMaxLen = instSize; m_patchBuf[0] = '\0'; m_showPatchPopup = true; break;
    case 4: m_patchAddr = addr; m_patchMaxLen = m_hexCols; m_patchBuf[0] = '\0'; m_showPatchPopup = true; break;
    case 5: DoNOP(addr, m_hexCols); break;
    case 6: m_hexAddr = addr; break;
    case 7: m_disasmAddr = addr; RefreshDisasm(); break;
    case 8: {
        char buf[32]; snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)addr);
        if (OpenClipboard(m_hwnd)) { EmptyClipboard(); HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, strlen(buf)+1);
        memcpy(GlobalLock(h), buf, strlen(buf)+1); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); CloseClipboard(); }
    } break;
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ============================================================
// Input
// ============================================================
void MemoryViewer::OnMouseDown(int x, int y, bool right, bool dbl) {
    m_ui.mouse = { x, y };
    m_ui.mousePressed = !right && !dbl;
    m_ui.mouseDoubleClicked = dbl;
    m_ui.mouseRightPressed = right;

    if (right) {
        // Determine if click is in disasm or hex area
        RECT rc; GetClientRect(m_hwnd, &rc);
        int halfH = (rc.bottom - 30 - 2) / 2;
        int disasmTop = 30 + 2;
        int hexTop = disasmTop + halfH + 2;

        if (y >= disasmTop && y < disasmTop + halfH) {
            // Disasm click - find instruction
            int lineH = 16;
            int idx = (y - disasmTop) / lineH;
            if (idx >= 0 && idx < (int)m_disasmView.size()) {
                ShowContextMenu(x, y, m_disasmView[idx].address, true, m_disasmView[idx].size);
            }
        } else if (y >= hexTop) {
            int lineH = 16;
            int line = (y - hexTop) / lineH;
            uintptr_t addr = m_hexAddr + line * m_hexCols;
            ShowContextMenu(x, y, addr, false, 0);
        }
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MemoryViewer::OnMouseWheel(int delta) {
    RECT rc; GetClientRect(m_hwnd, &rc);
    int halfH = (rc.bottom - 30 - 2) / 2;
    int disasmTop = 30 + 2;
    int hexTop = disasmTop + halfH + 2;

    if (m_ui.mouse.y < hexTop) {
        ScrollDisasm(delta > 0 ? -1 : 1);
    } else {
        ScrollHex(delta > 0 ? -1 : 1);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MemoryViewer::OnChar(wchar_t ch) {
    m_ui.charInput = ch;
    m_ui.hasCharInput = true;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MemoryViewer::OnKeyDown(WPARAM key) {
    m_ui.keyCode = (int)key;
    m_ui.keyPressed = true;
    m_ui.keyCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ============================================================
// Rendering
// ============================================================
void MemoryViewer::OnPaint(Gdiplus::Graphics* g, int w, int h) {
    m_ui.g = g;
    m_ui.width = w;
    m_ui.height = h;

    // Background
    UI::FillRect(g, {0, 0, w, h}, Theme::BG_MAIN());

    // Neon border
    UI::DrawNeonBorder(g, 0, 0, w - 1, h - 1, Theme::NEON());

    RECT addrBar = { 0, 0, w, 30 };
    RenderAddrBar(g, addrBar);

    UI::DrawSeparator(g, 30, 2, w - 2);

    int halfH = (h - 30 - 4) / 2;
    RECT disasmRc = { 2, 32, w - 2, 32 + halfH };
    RenderDisasm(g, disasmRc);

    UI::DrawSeparator(g, 32 + halfH, 2, w - 2);

    RECT hexRc = { 2, 34 + halfH, w - 2, h - 2 };
    RenderHex(g, hexRc);

    if (m_showPatchPopup) {
        RECT popupRc = { w/2 - 175, h/2 - 60, w/2 + 175, h/2 + 60 };
        RenderPatchPopup(g, popupRc);
    }

    // Reset input flags at the END of the frame so all widgets can
    // process this frame's input before it is cleared.
    m_ui.mousePressed = false;
    m_ui.mouseReleased = false;
    m_ui.mouseDoubleClicked = false;
    m_ui.mouseRightPressed = false;
    m_ui.hasCharInput = false;
    m_ui.keyPressed = false;
    m_ui.mouseWheel = 0;
}

void MemoryViewer::RenderAddrBar(Gdiplus::Graphics* g, RECT& rc) {
    UI::FillRect(g, rc, Theme::BG_TITLE());

    if (!m_pm || !m_pm->IsOpen()) {
        UI::DrawText(g, 8, 8, "No process selected", Theme::CLR_DIM());
        return;
    }

    UI::DrawText(g, 8, 8, "Address: 0x", Theme::CLR_TEXT());

    RECT inputRc = { 80, 5, 280, 25 };
    bool focused = m_addrBarFocus;
    bool dummyFocus = m_addrBarFocus;

    // Handle focus
    if (m_ui.PtInRect(inputRc) && m_ui.mousePressed) {
        m_addrBarFocus = true;
        m_ui.focusId = 1000;
    }
    if (m_addrBarFocus) m_ui.focusId = 1000;

    UI::TextInput(m_ui, 1000, inputRc, m_addrBuf, sizeof(m_addrBuf));

    RECT goBtn = { 285, 4, 320, 26 };
    if (UI::Button(m_ui, 1001, goBtn, "Go")) {
        ParseAndGo();
    }
    RECT sync1 = { 325, 4, 420, 26 };
    if (UI::Button(m_ui, 1002, sync1, "Sync Hex")) {
        m_hexAddr = m_disasmAddr;
    }
    RECT sync2 = { 425, 4, 530, 26 };
    if (UI::Button(m_ui, 1003, sync2, "Sync Disasm")) {
        m_disasmAddr = m_hexAddr;
        RefreshDisasm();
    }
}

void MemoryViewer::RenderDisasm(Gdiplus::Graphics* g, RECT& rc) {
    UI::FillRect(g, rc, Theme::BG_PANEL());

    if (!m_pm || !m_pm->IsOpen()) {
        UI::DrawText(g, rc.left + 4, rc.top + 4, "No process", Theme::CLR_DIM());
        return;
    }
    if (!m_dis || !m_dis->IsInitialized()) {
        UI::DrawText(g, rc.left + 4, rc.top + 4, "Disassembler not initialized", Theme::CLR_DIM());
        return;
    }
    if (m_disasmView.empty()) RefreshDisasm();

    int y = rc.top + 2;
    int lineH = 16;
    char line[512];

    for (size_t i = 0; i < m_disasmView.size() && y < rc.bottom; i++) {
        auto& inst = m_disasmView[i];
        int pos = snprintf(line, sizeof(line), "%016llX  ", (unsigned long long)inst.address);
        int bl = std::min((int)inst.size, 8);
        for (int j = 0; j < bl; j++) pos += snprintf(line+pos, sizeof(line)-pos, "%02X ", inst.bytes[j]);
        for (int j = bl; j < 8; j++) pos += snprintf(line+pos, sizeof(line)-pos, "   ");
        while (pos < 52) line[pos++] = ' ';
        pos += snprintf(line+pos, sizeof(line)-pos, "%s %s", inst.mnemonic, inst.opStr);

        Gdiplus::Color col = (inst.address == m_selectedAddr) ? Theme::BG_SELECTED() : Theme::BG_PANEL();
        RECT lineRc = { rc.left, y, rc.right, y + lineH };
        UI::FillRect(g, lineRc, col);
        UI::DrawText(g, rc.left + 2, y + 1, line, Theme::CLR_TEXT());

        // Double-click to follow in hex
        if (m_ui.PtInRect(lineRc) && m_ui.mouseDoubleClicked) {
            m_hexAddr = inst.address;
        }
        if (m_ui.PtInRect(lineRc) && m_ui.mousePressed) {
            m_selectedAddr = inst.address;
        }

        y += lineH;
    }
}

void MemoryViewer::RenderHex(Gdiplus::Graphics* g, RECT& rc) {
    UI::FillRect(g, rc, Theme::BG_PANEL());

    if (!m_pm || !m_pm->IsOpen()) {
        UI::DrawText(g, rc.left + 4, rc.top + 4, "No process", Theme::CLR_DIM());
        return;
    }

    int totalBytes = m_hexLines * m_hexCols;
    std::vector<uint8_t> buf(totalBytes);
    bool ok = m_pm->Read(m_hexAddr, buf.data(), totalBytes);

    if (!ok) {
        char err[128];
        snprintf(err, sizeof(err), "?? Failed to read at 0x%llX", (unsigned long long)m_hexAddr);
        UI::DrawText(g, rc.left + 4, rc.top + 4, err, Theme::CLR_RED());
        return;
    }

    int y = rc.top + 2;
    int lineH = 16;
    char out[512];

    for (int line = 0; line < m_hexLines && y < rc.bottom; line++) {
        uintptr_t lineAddr = m_hexAddr + (uintptr_t)(line * m_hexCols);
        int pos = snprintf(out, sizeof(out), "%016llX  ", (unsigned long long)lineAddr);
        for (int col = 0; col < m_hexCols; col++)
            pos += snprintf(out+pos, sizeof(out)-pos, "%02X ", buf[line*m_hexCols+col]);
        out[pos++] = ' '; out[pos++] = '|';
        for (int col = 0; col < m_hexCols; col++) {
            char c = (char)buf[line*m_hexCols+col];
            out[pos++] = (c >= 32 && c < 127) ? c : '.';
        }
        out[pos++] = '|'; out[pos] = '\0';

        UI::DrawText(g, rc.left + 2, y + 1, out, Theme::CLR_TEXT());
        y += lineH;
    }
}

void MemoryViewer::RenderPatchPopup(Gdiplus::Graphics* g, RECT& rc) {
    if (!m_showPatchPopup) return;
    UI::FillRect(g, rc, Theme::BG_PANEL());
    UI::DrawNeonBorder(g, rc.left, rc.top, rc.right - rc.left - 1, rc.bottom - rc.top - 1, Theme::NEON());

    char label[128];
    snprintf(label, sizeof(label), "Patch at 0x%llX (max %zu bytes)", (unsigned long long)m_patchAddr, m_patchMaxLen);
    UI::DrawText(g, rc.left + 8, rc.top + 4, label, Theme::CLR_TEXT());

    RECT inputRc = { rc.left + 8, rc.top + 22, rc.right - 110, rc.top + 42 };
    UI::TextInput(m_ui, 2000, inputRc, m_patchBuf, sizeof(m_patchBuf));

    RECT patchBtn = { rc.right - 100, rc.top + 20, rc.right - 60, rc.top + 42 };
    if (UI::Button(m_ui, 2001, patchBtn, "Patch")) {
        DoPatch(m_patchAddr, m_patchBuf);
        m_showPatchPopup = false;
        m_patchBuf[0] = '\0';
    }
    RECT cancelBtn = { rc.right - 55, rc.top + 20, rc.right - 8, rc.top + 42 };
    if (UI::Button(m_ui, 2002, cancelBtn, "Cancel")) {
        m_showPatchPopup = false;
        m_patchBuf[0] = '\0';
    }
}
