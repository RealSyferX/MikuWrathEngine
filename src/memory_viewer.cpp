#include "memory_viewer.h"
#include <windowsx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
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

void MemoryViewer::ProcessPendingActions() {
    if (m_ui.pendingComboId != -1) {
        HMENU menu = CreatePopupMenu();
        for (int i = 0; i < m_ui.pendingComboCount; i++) {
            AppendMenuA(menu, MF_STRING | (i == m_ui.pendingComboSelected ? MF_CHECKED : 0),
                i + 1, m_ui.pendingComboItems[i]);
        }
        POINT pt = { m_ui.pendingComboRc.left, m_ui.pendingComboRc.bottom };
        ClientToScreen(m_hwnd, &pt);
        int result = (int)TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
            pt.x, pt.y, 0, m_hwnd, nullptr);
        DestroyMenu(menu);
        if (result > 0 && m_ui.pendingComboSelectedPtr) {
            *m_ui.pendingComboSelectedPtr = result - 1;
        }
        m_ui.pendingComboId = -1;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

LRESULT MemoryViewer::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        // Guard against re-entrant paints (modal loops dispatched from
        // ProcessPendingActions or ShowContextMenu can trigger WM_PAINT).
        static bool s_inPaint = false;
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

        // Zero-size window — CreateCompatibleBitmap(hdc, 0, 0) returns NULL.
        if (w <= 0 || h <= 0) {
            EndPaint(m_hwnd, &ps);
            s_inPaint = false;
            return 0;
        }

        // Double buffer
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBmp = memBmp ? (HBITMAP)SelectObject(memDC, memBmp) : nullptr;

        if (memDC && memBmp) {
            {
                Gdiplus::Graphics g(memDC);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                OnPaint(&g, w, h);
            }
            // Clear dangling pointer — Graphics is destroyed above.
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
        return 0;
    case WM_LBUTTONUP:
        m_ui.mouseDown = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDBLCLK:
        OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), false, true);
        return 0;
    case WM_RBUTTONDOWN:
        OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), true, false);
        return 0;
    case WM_MOUSEMOVE: {
        m_ui.mouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
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
    case WM_GETMINMAXINFO: {
        // Prevent the memory viewer from being resized below a usable size.
        LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
        mmi->ptMinTrackSize.x = 400;
        mmi->ptMinTrackSize.y = 300;
        return 0;
    }
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
    if (!m_pm || !m_pm->IsOpen()) return;
    uintptr_t val = m_pm->ParseAddressString(m_addrBuf);
    m_hexAddr = val;
    m_disasmAddr = val;
    RefreshDisasm();
}

void MemoryViewer::RefreshDisasm() {
    m_disasmView.clear();
    if (!m_pm || !m_pm->IsOpen() || !m_dis || !m_dis->IsInitialized()) return;
    uint8_t buf[1024];
    if (m_pm->Read(m_disasmAddr, buf, sizeof(buf))) {
        m_disasmView = m_dis->Disassemble(m_disasmAddr, buf, sizeof(buf), 64);
    }
}

void MemoryViewer::ScrollHex(int lines) {
    int absLines = (lines < 0) ? -lines : lines;
    uintptr_t delta = (uintptr_t)(absLines * m_hexCols);
    if (lines < 0) {
        if (m_hexAddr >= delta) m_hexAddr -= delta;
        else m_hexAddr = 0;
    } else {
        m_hexAddr += delta;
    }
}

void MemoryViewer::ScrollDisasm(int lines) {
    if (!m_dis || !m_dis->IsInitialized()) return;
    if (!m_pm || !m_pm->IsOpen()) return;
    int count = std::min(std::abs(lines), 50);
    if (lines > 0) {
        for (int i = 0; i < count; i++) {
            if (m_disasmView.size() > 1) {
                m_disasmAddr = m_disasmView[1].address;
                m_disasmView.erase(m_disasmView.begin());
                // Refill cache when running low (threshold 16, read 32 more)
                if (m_disasmView.size() < 16 && !m_disasmView.empty()) {
                    uintptr_t nextAddr = m_disasmView.back().address + m_disasmView.back().size;
                    uint8_t buf[1024];
                    if (m_pm->Read(nextAddr, buf, sizeof(buf))) {
                        auto more = m_dis->Disassemble(nextAddr, buf, sizeof(buf), 32);
                        m_disasmView.insert(m_disasmView.end(), more.begin(), more.end());
                    }
                }
            } else if (m_disasmView.size() == 1) {
                // Only one instruction left — advance past it and re-read
                m_disasmAddr = m_disasmView[0].address + m_disasmView[0].size;
                m_disasmView.clear();
                uint8_t buf[1024];
                if (m_pm->Read(m_disasmAddr, buf, sizeof(buf))) {
                    m_disasmView = m_dis->Disassemble(m_disasmAddr, buf, sizeof(buf), 64);
                }
            } else {
                break; // empty, can't scroll
            }
        }
    } else if (lines < 0) {
        for (int i = 0; i < count; i++) {
            uintptr_t prevAddr = m_dis->FindPreviousInstruction(m_disasmAddr, m_pm);
            if (prevAddr != m_disasmAddr) {
                m_disasmAddr = prevAddr;
            }
        }
        RefreshDisasm();
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
        AppendMenuA(menu, MF_STRING, 9, "AOB Signature...");
        AppendMenuA(menu, MF_STRING, 10, "Assemble...");
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
        CopyToClipboard(m_hwnd, buf);
    } break;
    case 9: {
        // AOB Signature maker
        m_sigAddr = addr;
        m_sigLen = std::min((int)instSize * 4, MAX_SIG_BYTES);
        if (m_sigLen < 8) m_sigLen = 16;
        if (m_pm && m_pm->IsOpen()) {
            m_pm->Read(m_sigAddr, m_sigBytes, m_sigLen);
        }
        memset(m_sigWildcard, 0, sizeof(m_sigWildcard));
        m_showSigMaker = true;
    } break;
    case 10: {
        // Assemble / change opcode dialog
        m_asmAddr = addr;
        m_asmMaxLen = instSize;
        m_asmBuf[0] = '\0';
        m_asmError[0] = '\0';
        // Find current instruction text
        for (auto& inst : m_disasmView) {
            if (inst.address == addr) {
                snprintf(m_asmCurrentInst, sizeof(m_asmCurrentInst), "%s %s",
                         inst.mnemonic, inst.opStr);
                break;
            }
        }
        m_showAsmPopup = true;
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
    m_ui.mouseDown = !right;
    m_ui.mouseDoubleClicked = dbl;
    m_ui.mouseRightPressed = right;

    if (right) {
        // Determine if click is in disasm or hex area
        RECT rc; GetClientRect(m_hwnd, &rc);
        int minPanelH = 40;
        int halfH = std::max(minPanelH, (int)(rc.bottom - 30 - 2) / 2);
        int disasmTop = 30 + 2;
        int hexTop = disasmTop + halfH + 2;

        if (y >= disasmTop && y < disasmTop + halfH) {
            // Disasm click - find instruction
            int lineH = std::max(16, UI::g_fontSize + 7);
            int idx = (y - disasmTop) / lineH;
            if (idx >= 0 && idx < (int)m_disasmView.size()) {
                ShowContextMenu(x, y, m_disasmView[idx].address, true, m_disasmView[idx].size);
            }
        } else if (y >= hexTop) {
            int lineH = std::max(16, UI::g_fontSize + 7);
            int line = (y - hexTop) / lineH;
            uintptr_t addr = m_hexAddr + line * m_hexCols;
            ShowContextMenu(x, y, addr, false, 0);
        }
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MemoryViewer::OnMouseWheel(int delta) {
    RECT rc; GetClientRect(m_hwnd, &rc);
    int minPanelH = 40;
    int halfH = std::max(minPanelH, (int)(rc.bottom - 30 - 2) / 2);
    int disasmTop = 30 + 2;
    int hexTop = disasmTop + halfH + 2;

    if (m_ui.mouse.y < hexTop) {
        ScrollDisasm(delta > 0 ? -3 : 3);
    } else {
        ScrollHex(delta > 0 ? -3 : 3);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MemoryViewer::OnChar(wchar_t ch) {
    if (m_hexEditing && m_hexSelLine >= 0 && m_hexSelCol >= 0 &&
        m_hexSelLine < m_hexLines && m_hexSelCol < m_hexCols) {
        if (isxdigit((int)ch)) {
            int val = (ch >= '0' && ch <= '9') ? ch - '0' :
                      (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : ch - 'a' + 10;

            uintptr_t addr = m_hexAddr + (uintptr_t)(m_hexSelLine * m_hexCols + m_hexSelCol);
            uint8_t current = 0;
            if (m_pm && m_pm->IsOpen() && m_pm->Read(addr, &current, 1)) {
                if (m_hexNibble == 0) {
                    current = (uint8_t)((current & 0x0F) | (val << 4));
                } else {
                    current = (uint8_t)((current & 0xF0) | val);
                }
                m_pm->Write(addr, &current, 1);

                m_hexNibble++;
                if (m_hexNibble >= 2) {
                    m_hexNibble = 0;
                    m_hexSelCol++;
                    if (m_hexSelCol >= m_hexCols) {
                        m_hexSelCol = 0;
                        m_hexSelLine++;
                        if (m_hexSelLine >= m_hexLines) {
                            ScrollHex(1);
                            m_hexSelLine = m_hexLines - 1;
                        }
                    }
                }
            }
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        // Non-hex char while editing: fall through to let widgets handle it
        // (e.g. Ctrl+C for copy won't arrive via WM_CHAR anyway)
    }

    m_ui.charInput = ch;
    m_ui.hasCharInput = true;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MemoryViewer::OnKeyDown(WPARAM key) {
    // Hex editing mode: handle navigation before anything else
    if (m_hexEditing) {
        switch (key) {
        case VK_LEFT:
            if (m_hexSelCol > 0) {
                m_hexSelCol--;
            } else if (m_hexSelLine > 0) {
                m_hexSelCol = m_hexCols - 1;
                m_hexSelLine--;
            }
            m_hexNibble = 0;
            break;
        case VK_RIGHT:
            if (m_hexSelCol < m_hexCols - 1) {
                m_hexSelCol++;
            } else {
                m_hexSelCol = 0;
                m_hexSelLine++;
                if (m_hexSelLine >= m_hexLines) {
                    ScrollHex(1);
                    m_hexSelLine = m_hexLines - 1;
                }
            }
            m_hexNibble = 0;
            break;
        case VK_UP:
            if (m_hexSelLine > 0) m_hexSelLine--;
            m_hexNibble = 0;
            break;
        case VK_DOWN:
            m_hexSelLine++;
            if (m_hexSelLine >= m_hexLines) {
                ScrollHex(1);
                m_hexSelLine = m_hexLines - 1;
            }
            m_hexNibble = 0;
            break;
        case VK_ESCAPE:
            m_hexEditing = false;
            m_hexSelLine = -1;
            m_hexSelCol = -1;
            break;
        case VK_TAB:
            m_hexSelCol++;
            if (m_hexSelCol >= m_hexCols) {
                m_hexSelCol = 0;
                m_hexSelLine++;
                if (m_hexSelLine >= m_hexLines) {
                    ScrollHex(1);
                    m_hexSelLine = m_hexLines - 1;
                }
            }
            m_hexNibble = 0;
            break;
        case VK_HOME:
            m_hexSelCol = 0;
            m_hexNibble = 0;
            break;
        case VK_END:
            m_hexSelCol = m_hexCols - 1;
            m_hexNibble = 0;
            break;
        default:
            // Let other keys fall through to normal processing
            // only if not a navigation/editing key
            break;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    m_ui.keyCode = (int)key;
    m_ui.keyPressed = true;
    m_ui.keyCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (m_ui.keyCtrl && key == 'G') {
        m_addrBarFocus = true;
        m_ui.focusId = 1000;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Only scroll if not editing text and no modal popup is open
    if (m_ui.focusId == 1000 || m_showPatchPopup || m_showSigMaker || m_showAsmPopup) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return; // text input focused or popup open
    }

    switch (key) {
    case VK_UP:    ScrollDisasm(-1); break;
    case VK_DOWN:  ScrollDisasm(1); break;
    case VK_PRIOR: ScrollDisasm(-10); break;
    case VK_NEXT:  ScrollDisasm(10); break;
    case VK_HOME:  ScrollDisasm(-50); break;
    case VK_END:   ScrollDisasm(50); break;
    }
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

    // Ensure each panel has a minimum usable height even on short windows.
    int minPanelH = 40;
    int halfH = std::max(minPanelH, (h - 30 - 4) / 2);
    RECT disasmRc = { 2, 32, w - 2, 32 + halfH };
    RenderDisasm(g, disasmRc);

    UI::DrawSeparator(g, 32 + halfH, 2, w - 2);

    RECT hexRc = { 2, 34 + halfH, w - 2, h - 2 };
    RenderHex(g, hexRc);

    if (m_showPatchPopup) {
        int ppW = std::min(350, std::max(280, w - 20));
        int ppH = std::min(120, std::max(60, h - 20));
        RECT popupRc = { w/2 - ppW/2, h/2 - ppH/2, w/2 + ppW/2, h/2 + ppH/2 };
        RenderPatchPopup(g, popupRc);
    }

    if (m_showSigMaker) {
        int smW = std::min(500, std::max(360, w - 20));
        int smH = std::min(200, std::max(180, h - 20));
        RECT sigRc = { w/2 - smW/2, h/2 - smH/2, w/2 + smW/2, h/2 + smH/2 };
        RenderSigMaker(g, sigRc);
    }

    if (m_showAsmPopup) {
        int apW = std::min(400, std::max(300, w - 20));
        int apH = std::min(140, std::max(130, h - 20));
        RECT asmRc = { w/2 - apW/2, h/2 - apH/2, w/2 + apW/2, h/2 + apH/2 };
        RenderAsmPopup(g, asmRc);
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

    int w = (int)(rc.right - rc.left);

    if (!m_pm || !m_pm->IsOpen()) {
        UI::DrawText(g, 8, 8, "No process selected", Theme::CLR_DIM());
        return;
    }

    UI::DrawText(g, 8, 8, "Address: 0x", Theme::CLR_TEXT());

    // Input field shrinks when the window is narrow so the buttons always
    // remain visible to its right.  Minimum input width is 60px.
    int inputLeft = 80;
    int goBtnW = 35, syncHexW = 95, syncDisasmW = 105;
    int btnGap = 5;
    int buttonsTotalW = goBtnW + syncHexW + syncDisasmW + btnGap * 2;
    int inputRight = std::max(inputLeft + 60, w - buttonsTotalW - 8);
    RECT inputRc = { inputLeft, 5, inputRight, 25 };
    bool focused = m_addrBarFocus;
    bool dummyFocus = m_addrBarFocus;

    // Handle focus
    if (m_ui.PtInRect(inputRc) && m_ui.mousePressed) {
        m_addrBarFocus = true;
        m_ui.focusId = 1000;
        m_hexEditing = false;  // clicking the address bar exits hex editing
    }
    if (m_addrBarFocus) m_ui.focusId = 1000;

    UI::TextInput(m_ui, 1000, inputRc, m_addrBuf, sizeof(m_addrBuf));

    // Buttons flow immediately after the input field.
    int bx = inputRight + btnGap;
    RECT goBtn = { bx, 4, bx + goBtnW, 26 };
    if (UI::Button(m_ui, 1001, goBtn, "Go")) {
        ParseAndGo();
    }
    bx += goBtnW + btnGap;
    RECT sync1 = { bx, 4, bx + syncHexW, 26 };
    if (UI::Button(m_ui, 1002, sync1, "Sync Hex")) {
        m_hexAddr = m_disasmAddr;
    }
    bx += syncHexW + btnGap;
    RECT sync2 = { bx, 4, bx + syncDisasmW, 26 };
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
    int lineH = std::max(16, UI::g_fontSize + 7);
    // Only render as many instructions as fit in the available height.
    int maxLines = std::max(1, (int)(rc.bottom - rc.top - 4) / lineH);
    char line[512];

    for (size_t i = 0; i < m_disasmView.size() && (int)i < maxLines && y < rc.bottom; i++) {
        auto& inst = m_disasmView[i];
        // Module-relative address, padded to 30 chars + 2 spaces
        std::string addrStr = m_pm->FormatAddress(inst.address);
        int pos = snprintf(line, sizeof(line), "%-30.30s  ", addrStr.c_str());
        int bl = std::min((int)inst.size, 8);
        for (int j = 0; j < bl; j++) pos += snprintf(line+pos, sizeof(line)-pos, "%02X ", inst.bytes[j]);
        for (int j = bl; j < 8; j++) pos += snprintf(line+pos, sizeof(line)-pos, "   ");
        while (pos < 64) line[pos++] = ' ';
        pos += snprintf(line+pos, sizeof(line)-pos, "%s %s", inst.mnemonic, inst.opStr);

        Gdiplus::Color col = (inst.address == m_selectedAddr) ? Theme::BG_SELECTED() : Theme::BG_PANEL();
        RECT lineRc = { rc.left, y, rc.right, y + lineH };
        UI::FillRect(g, lineRc, col);
        UI::DrawText(g, rc.left + 2, y + 1, line, Theme::CLR_TEXT());

        // Double-click to follow in hex (disabled when modal popups are open)
        if (!m_showPatchPopup && !m_showSigMaker && !m_showAsmPopup &&
            m_ui.PtInRect(lineRc) && m_ui.mouseDoubleClicked) {
            m_hexAddr = inst.address;
        }
        if (!m_showPatchPopup && !m_showSigMaker && !m_showAsmPopup &&
            m_ui.PtInRect(lineRc) && m_ui.mousePressed) {
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

    // Dynamically size the number of hex lines to the available panel height
    // so we never read or render more rows than fit on screen.
    int lineH = std::max(16, UI::g_fontSize + 7);
    int availH = (int)(rc.bottom - rc.top) - 4;
    m_hexLines = std::max(1, availH / lineH);

    int totalBytes = m_hexLines * m_hexCols;
    std::vector<uint8_t> buf(totalBytes);
    bool ok = m_pm->Read(m_hexAddr, buf.data(), totalBytes);

    if (!ok) {
        char err[128];
        snprintf(err, sizeof(err), "?? Failed to read at 0x%llX", (unsigned long long)m_hexAddr);
        UI::DrawText(g, rc.left + 4, rc.top + 4, err, Theme::CLR_RED());
        return;
    }

    // Measure character width for hit-testing and highlight positioning.
    // Consolas 9px is monospaced, so every glyph shares the same advance width.
    // Use 32-char sample for accuracy with the wider 30-char address column.
    int refW = 0, refH = 0;
    UI::MeasureText(g, "00000000000000000000000000000000", nullptr, &refW, &refH); // 32 chars
    int charW = (refW > 0) ? (refW + 16) / 32 : 6; // round, fallback to 6px

    // Layout: "ADDR  " (32 chars = 30 padded address + 2 spaces) | hex bytes ("XX " = 3 chars each) | " |" (2 chars) | ASCII (1 char each) | "|"
    int textX = rc.left + 2;
    int hexStartX = textX + 32 * charW;               // address prefix = 30 chars + 2 spaces
    int byteW = 3 * charW;                            // each byte = "XX "
    int sepW = 2 * charW;                              // " |" (space + pipe before ASCII)
    int asciiStartX = hexStartX + m_hexCols * byteW + sepW;

    int y = rc.top + 2;
    char out[512];

    for (int line = 0; line < m_hexLines && y < rc.bottom; line++) {
        uintptr_t lineAddr = m_hexAddr + (uintptr_t)(line * m_hexCols);
        // Module-relative address, padded to 30 chars + 2 spaces
        std::string addrStr = m_pm->FormatAddress(lineAddr);
        int pos = snprintf(out, sizeof(out), "%-30.30s  ", addrStr.c_str());
        for (int col = 0; col < m_hexCols; col++)
            pos += snprintf(out+pos, sizeof(out)-pos, "%02X ", buf[line*m_hexCols+col]);
        out[pos++] = ' '; out[pos++] = '|';
        for (int col = 0; col < m_hexCols; col++) {
            char c = (char)buf[line*m_hexCols+col];
            out[pos++] = (c >= 32 && c < 127) ? c : '.';
        }
        out[pos++] = '|'; out[pos] = '\0';

        // Draw selection highlight BEFORE text so the text renders on top
        if (m_hexEditing && line == m_hexSelLine &&
            m_hexSelCol >= 0 && m_hexSelCol < m_hexCols) {
            int byteX = hexStartX + m_hexSelCol * byteW;
            // Highlight the 2 hex chars (not the trailing space)
            RECT selRc = { byteX - 1, y, byteX + 2 * charW, y + lineH };
            UI::FillRect(g, selRc, Theme::BG_SELECTED());

            // Mirror the highlight in the ASCII column
            int asciiX = asciiStartX + m_hexSelCol * charW;
            RECT asciiSelRc = { asciiX - 1, y, asciiX + charW, y + lineH };
            UI::FillRect(g, asciiSelRc, Theme::BG_SELECTED());

            // Nibble caret — blinks with the caret timer
            if (m_caretBlink) {
                int caretX = (m_hexNibble == 0) ? byteX : byteX + charW;
                Gdiplus::Pen pen(Theme::NEON(), 1.0f);
                g->DrawLine(&pen, caretX, y + 1, caretX, y + lineH - 1);
            }
        }

        UI::DrawText(g, textX, y + 1, out, Theme::CLR_TEXT());

        // Click-to-select: compute which byte was clicked (hex or ASCII column)
        // Disabled when modal popups are open to prevent background selection
        if (m_ui.mousePressed && !m_showPatchPopup && !m_showSigMaker && !m_showAsmPopup) {
            int mx = m_ui.mouse.x;
            int my = m_ui.mouse.y;
            if (my >= y && my < y + lineH) {
                int clickedCol = -1;
                if (mx >= hexStartX && mx < hexStartX + m_hexCols * byteW) {
                    clickedCol = (mx - hexStartX) / byteW;
                } else if (mx >= asciiStartX && mx < asciiStartX + m_hexCols * charW) {
                    clickedCol = (mx - asciiStartX) / charW;
                }
                if (clickedCol >= 0 && clickedCol < m_hexCols) {
                    m_hexSelLine = line;
                    m_hexSelCol = clickedCol;
                    m_hexEditing = true;
                    m_hexNibble = 0;
                    m_addrBarFocus = false;
                    m_ui.focusId = -1;
                }
            }
        }

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

// ============================================================
// AOB Signature Maker — build byte signatures with wildcards
// ============================================================
void MemoryViewer::RenderSigMaker(Gdiplus::Graphics* g, RECT& rc) {
    if (!m_showSigMaker) return;

    UI::FillRect(g, rc, Theme::BG_PANEL());
    UI::DrawNeonBorder(g, rc.left, rc.top, rc.right - rc.left - 1,
                       rc.bottom - rc.top - 1, Theme::NEON());

    // Title with module-relative address
    char title[256];
    if (m_pm && m_pm->IsOpen()) {
        snprintf(title, sizeof(title), "AOB Signature at %s",
                 m_pm->FormatAddress(m_sigAddr).c_str());
    } else {
        snprintf(title, sizeof(title), "AOB Signature Maker");
    }
    UI::DrawText(g, rc.left + 8, rc.top + 4, title, Theme::CLR_TEXT());

    // Length controls
    UI::DrawText(g, rc.left + 8, rc.top + 24, "Len:", Theme::CLR_DIM());
    RECT minusBtn = { rc.left + 42, rc.top + 22, rc.left + 62, rc.top + 40 };
    if (UI::Button(m_ui, 4000, minusBtn, "-")) {
        if (m_sigLen > 4) m_sigLen--;
    }
    char lenStr[16];
    snprintf(lenStr, sizeof(lenStr), "%d", m_sigLen);
    UI::DrawText(g, rc.left + 70, rc.top + 24, lenStr, Theme::CLR_TEXT());
    RECT plusBtn = { rc.left + 90, rc.top + 22, rc.left + 110, rc.top + 40 };
    if (UI::Button(m_ui, 4001, plusBtn, "+")) {
        if (m_sigLen < MAX_SIG_BYTES) m_sigLen++;
    }

    // Refresh button (re-read is also done automatically each frame)
    RECT refreshBtn = { rc.right - 90, rc.top + 22, rc.right - 10, rc.top + 40 };
    if (UI::Button(m_ui, 4002, refreshBtn, "Refresh")) {
        /* Bytes are re-read below each frame */
    }

    // Re-read bytes each frame to keep the display live
    if (m_pm && m_pm->IsOpen()) {
        m_pm->Read(m_sigAddr, m_sigBytes, m_sigLen);
    }

    // Byte grid — 16 per row, each cell clickable to toggle wildcard
    int cellW = 28;
    int cellH = 16;
    int gridX = rc.left + 8;
    int gridY = rc.top + 46;
    int bytesPerRow = 16;

    for (int i = 0; i < m_sigLen; i++) {
        int row = i / bytesPerRow;
        int col = i % bytesPerRow;
        int x = gridX + col * cellW;
        int y = gridY + row * cellH;

        RECT cellRc = { x, y, x + cellW - 2, y + cellH };
        bool hovered = m_ui.PtInRect(cellRc);

        Gdiplus::Color bg = m_sigWildcard[i] ? Theme::BG_SELECTED() :
                            (hovered ? Theme::BG_HOVER() : Theme::BG_CONTROL());
        UI::FillRect(g, cellRc, bg);
        UI::DrawRect(g, cellRc, Theme::BORDER());

        char byteStr[4];
        if (m_sigWildcard[i]) {
            snprintf(byteStr, sizeof(byteStr), "??");
        } else {
            snprintf(byteStr, sizeof(byteStr), "%02X", m_sigBytes[i]);
        }
        Gdiplus::Color txtCol = m_sigWildcard[i] ? Theme::CLR_YELLOW() : Theme::CLR_TEXT();
        UI::DrawText(g, x + 5, y + 1, byteStr, txtCol);

        // Click toggles wildcard
        if (hovered && m_ui.mousePressed) {
            m_sigWildcard[i] = !m_sigWildcard[i];
        }
    }

    // Generate AOB string from bytes + wildcards
    std::string sig;
    for (int i = 0; i < m_sigLen; i++) {
        if (i > 0) sig += " ";
        if (m_sigWildcard[i]) {
            sig += "??";
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X", m_sigBytes[i]);
            sig += buf;
        }
    }
    strncpy(m_sigOutput, sig.c_str(), sizeof(m_sigOutput) - 1);
    m_sigOutput[sizeof(m_sigOutput) - 1] = '\0';

    // Show AOB string
    UI::DrawText(g, rc.left + 8, rc.top + 120, "Signature:", Theme::CLR_DIM());
    UI::DrawText(g, rc.left + 70, rc.top + 120, m_sigOutput, Theme::CLR_GREEN());

    // Copy button
    RECT copyBtn = { rc.right - 90, rc.top + 118, rc.right - 10, rc.top + 138 };
    if (UI::Button(m_ui, 4003, copyBtn, "Copy")) {
        CopyToClipboard(m_hwnd, m_sigOutput);
    }

    // Close button
    RECT closeBtn = { rc.right - 90, rc.top + 142, rc.right - 10, rc.top + 162 };
    if (UI::Button(m_ui, 4004, closeBtn, "Close")) {
        m_showSigMaker = false;
    }
}

// ============================================================
// Assemble Dialog — change opcode by patching hex bytes
// (No Keystone dependency; accepts hex like "90 90 EB 05")
// ============================================================
void MemoryViewer::RenderAsmPopup(Gdiplus::Graphics* g, RECT& rc) {
    if (!m_showAsmPopup) return;

    UI::FillRect(g, rc, Theme::BG_PANEL());
    UI::DrawNeonBorder(g, rc.left, rc.top, rc.right - rc.left - 1,
                       rc.bottom - rc.top - 1, Theme::NEON());

    // Title with module-relative address
    char label[256];
    if (m_pm && m_pm->IsOpen()) {
        snprintf(label, sizeof(label), "Assemble at %s",
                 m_pm->FormatAddress(m_asmAddr).c_str());
    } else {
        snprintf(label, sizeof(label), "Assemble");
    }
    UI::DrawText(g, rc.left + 8, rc.top + 4, label, Theme::CLR_TEXT());

    // Current instruction (for reference)
    UI::DrawText(g, rc.left + 8, rc.top + 22, "Current:", Theme::CLR_DIM());
    UI::DrawText(g, rc.left + 70, rc.top + 22, m_asmCurrentInst, Theme::CLR_YELLOW());

    // New bytes input (hex)
    UI::DrawText(g, rc.left + 8, rc.top + 44, "Bytes:", Theme::CLR_DIM());
    if (m_asmBuf[0] == '\0' && m_ui.focusId != 3000) {
        UI::DrawText(g, rc.left + 72, rc.top + 45, "e.g. 90 90 EB 05", Theme::CLR_DIM());
    }
    UI::TextInput(m_ui, 3000, {rc.left + 70, rc.top + 42, rc.right - 10, rc.top + 64},
                  m_asmBuf, sizeof(m_asmBuf));

    // Error display
    if (m_asmError[0]) {
        UI::DrawText(g, rc.left + 8, rc.top + 68, m_asmError, Theme::CLR_RED());
    }

    // Determine if we should assemble (button click or Enter key)
    bool doAssemble = false;
    RECT asmBtn = { rc.right - 190, rc.top + 90, rc.right - 100, rc.top + 112 };
    if (UI::Button(m_ui, 3001, asmBtn, "Assemble")) {
        doAssemble = true;
    }
    if (m_ui.focusId == 3000 && m_ui.keyPressed && m_ui.keyCode == VK_RETURN) {
        doAssemble = true;
    }

    if (doAssemble) {
        // Parse hex bytes and write to target process
        std::vector<uint8_t> bytes;
        std::istringstream iss(m_asmBuf);
        std::string tok;
        while (iss >> tok) {
            bytes.push_back((uint8_t)strtoul(tok.c_str(), nullptr, 16));
        }
        if (bytes.empty()) {
            snprintf(m_asmError, sizeof(m_asmError), "No valid bytes entered");
        } else if (!m_pm || !m_pm->IsOpen()) {
            snprintf(m_asmError, sizeof(m_asmError), "No process open");
        } else if (m_pm->Write(m_asmAddr, bytes.data(), bytes.size())) {
            m_showAsmPopup = false;
            m_asmBuf[0] = '\0';
            m_asmError[0] = '\0';
            RefreshDisasm();
        } else {
            snprintf(m_asmError, sizeof(m_asmError), "Write failed");
        }
    }

    // Cancel button
    RECT cancelBtn = { rc.right - 90, rc.top + 90, rc.right - 10, rc.top + 112 };
    if (UI::Button(m_ui, 3002, cancelBtn, "Cancel")) {
        m_showAsmPopup = false;
        m_asmBuf[0] = '\0';
        m_asmError[0] = '\0';
    }
}
