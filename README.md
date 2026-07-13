<div align="center">

<h1>⚡ MWE — MikuWrathEngine ⚡</h1>

### ⚡ A Cheat Engine clone built from scratch with native Win32 GDI+. No ImGui. No DirectX. Pure hand-rolled C++20. ⚡

> ### 🚀 **Standalone. Portable. Zero Install.**
> No setup. No installer. No runtime redistributables. No .NET, no Visual C++ runtime, no DirectX.
> Just download `MikuWrathEngine.exe` and **double-click to run**. That's it.

<p>
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white">
  <img alt="CMake" src="https://img.shields.io/badge/CMake-3.16+-064F8C?style=for-the-badge&logo=cmake&logoColor=white">
  <img alt="Win32" src="https://img.shields.io/badge/Win32-GDI%2B-007ACC?style=for-the-badge&logo=windows&logoColor=white">
  <img alt="Capstone" src="https://img.shields.io/badge/Capstone-5.0.3-8B0000?style=for-the-badge">
  <img alt="Platform" src="https://img.shields.io/badge/Platform-Windows%20x64-0078D4?style=for-the-badge&logo=windows10&logoColor=white">
  <img alt="License" src="https://img.shields.io/badge/License-MIT-00A8FF?style=for-the-badge">
</p>

<p>
  <a href="https://github.com/RealSyferX/MikuWrathEngine"><img alt="GitHub" src="https://img.shields.io/badge/GitHub-RealSyferX/MikuWrathEngine-181717?style=for-the-badge&logo=github&logoColor=white"></a>
  <a href="https://wrath.syferx.net"><img alt="Website" src="https://img.shields.io/badge/Web-wrath.syferx.net-00A8FF?style=for-the-badge&logo=internet-explorer&logoColor=white"></a>
</p>

---

</div>

<!-- screenshot here -->

## 🎯 What is MikuWrathEngine?

**MikuWrathEngine** is a fully-featured memory editor and debugger for Windows — a from-scratch **Cheat Engine clone** that refuses to take the easy way out. Instead of pulling in Dear ImGui or a DirectX swapchain, every pixel is painted by hand using the **Win32 API + GDI+**. Every widget — buttons, checkboxes, text inputs, combo boxes, scrollbars, hex grids, disassembly views — is an **immediate-mode** control rendered with `Gdiplus::Graphics`.

The result is a lean, dependency-light binary that runs on stock Windows with **zero runtime redistributables** and a gorgeous **VS2022-inspired dark theme** with **neon glow borders**.

> Built by a reverse engineer, for reverse engineers.

---

## 🚀 Quick Start

### Option A: Download (Zero Build)

1. Go to [Releases](https://github.com/RealSyferX/MikuWrathEngine/releases)
2. Download `MikuWrathEngine.exe`
3. Double-click. Done. ✅

> **No installation. No prerequisites. No admin rights needed (unless the target process requires elevation). The exe is fully self-contained — Capstone is statically linked, GDI+ ships with Windows, and everything else is hand-rolled Win32.**

### Option B: Build from Source

#### Prerequisites

- **Visual Studio 2022** (or Build Tools) with the **C++ workload**
- **CMake 3.16+**
- Windows 10/11 x64

> Capstone 5.0.3 is fetched automatically via CMake `FetchContent` — no manual install needed.

#### Build Script

```bat
:: Double-click or run in a terminal:
build.bat
```

Output: `build\Release\MikuWrathEngine.exe`

#### Manual CMake

```bash
# 1. Configure
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release

# 2. Build
cmake --build build --config Release --parallel
```

#### IDE — Visual Studio 2022

Open the project folder in **Visual Studio 2022** (it detects `CMakeLists.txt` automatically), select the `x64-Release` profile, and hit `F5`.

---

## ✨ Features

### 📦 Distribution & Portability

| Feature | Description |
|---------|-------------|
| 🪶 **Standalone EXE** | Single self-contained `.exe` — no installation, no DLLs, no redistributables. Capstone statically linked. Just run it. |

### 🔍 Process Management

| Feature | Description |
|---------|-------------|
| 📋 **Process Selector** | Live-sorted process list with real-time **search/filter** box |
| 🔗 **Auto-Attach** | Set a target process name (e.g. `MAT.exe`) and MikuWrath auto-attaches the moment it launches |
| 🏷️ **x86 / x64 Detection** | Automatically detects WoW64 processes and switches the disassembler mode |
| 📂 **Process Path Display** | Full executable path shown in the process bar |

### 🧲 Memory Scanner

A fully **asynchronous** (`std::thread`) scanner with progress bar, live result count, and cancel/stop support.

#### Scan Modes

| Mode | First Scan | Next Scan | Description |
|------|:----------:|:---------:|-------------|
| **Exact Value** | ✅ | ✅ | Match a specific value |
| **Bigger Than** | ✅ | ✅ | Value > target |
| **Smaller Than** | ✅ | ✅ | Value < target |
| **Between** | ✅ | ✅ | Value within a range (two inputs) |
| **Unknown Initial Value** | ✅ | — | Snapshot all memory, filter later |
| **Changed** | — | ✅ | Value differs from last scan |
| **Unchanged** | — | ✅ | Value same as last scan |
| **Increased** | — | ✅ | Value went up |
| **Decreased** | — | ✅ | Value went down |

#### Value Types

| Type | Size | Hex Mode | Notes |
|------|------|:--------:|-------|
| 🟦 **Byte** | 1 | ✅ | `uint8_t` |
| 🟩 **2 Bytes** | 2 | ✅ | `uint16_t` (Word) |
| 🟨 **4 Bytes** | 4 | ✅ | `uint32_t` (Dword) |
| 🟧 **8 Bytes** | 8 | ✅ | `uint64_t` (Qword) |
| 🔵 **Float** | 4 | ✅ | IEEE 754 single |
| 🟣 **Double** | 8 | ✅ | IEEE 754 double |
| 🔤 **String** | var | — | Null-terminated byte string |
| 🎯 **AOB** | var | — | Array of Bytes with **wildcards** — e.g. `7F ?? 90 41` |

#### Scanner Options

- ✅ **Hex mode** toggle for numeric types
- ✅ **Writable-only** filter (skip read-only/execute pages)
- ✅ **Scan region selection** — scan *All* memory or restrict to a specific module (e.g. `MAT.exe`)
- ✅ **Result cap** — handles up to 5,000,000 results (displays first 100,000)
- ✅ **Batched value reads** — result values refreshed 4×/sec instead of per-row RPM

### 📋 Address Table

| Feature | Description |
|---------|-------------|
| 🧊 **Freeze / Lock** | Lock values in place — re-written at 100 Hz |
| 💾 **Save / Load** | Persist tables to `.mwt` files (MWT2 format with type, address, frozen, value, description) |
| ➕ **Manual Add** | Add addresses by hex (`0x00400000`) or **module-relative** (`MAT.exe+85023`) |
| ✏️ **Inline Edit** | Edit description, type, and value directly in the table |
| 🗑️ **Per-Row Remove** | Delete individual entries |
| 🧹 **Clear All** | Wipe the entire table |
| 🖱️ **Right-Click Menu** | Browse in Memory Viewer, Copy Address, Copy Value, Delete |

### 🐞 Debugger

A built-in debug engine that attaches to the target and runs its own debug loop on a dedicated thread. Two backends are selectable via the `debugger_type` setting: **Windows** (Windows Debug API — INT3 + hardware breakpoints) or **VEH** (Vectored Exception Handler — hardware breakpoints only). Set to **None** to disable debugging entirely.

| Feature | Description |
|---------|-------------|
| 🔴 **Execute Breakpoints** | Break when an instruction at the address executes (`BreakType::Execute`) |
| ✍️ **Write Breakpoints** | Break when the address is written (`BreakType::Write`) |
| 👁️ **Access Breakpoints** | Break on read *or* write of the address (`BreakType::Access`) |
| ⚙️ **Hardware Breakpoints** | Debug-register breakpoints across four slots (**DR0–DR3**), applied per-thread |
| 🧩 **INT3 Breakpoints** | Software breakpoints via `0xCC` byte patching (Windows backend), with original-byte restore |
| ⏭️ **Step Into / Step Over / Continue** | Single-step, step over calls (temporary breakpoint), and resume execution |
| 🧵 **Register Snapshot** | Captures RAX–R15, RIP, and EFLAGS at the point of halt |
| 🔎 **Find What Accesses** | Set an access watch on an address and collect every instruction that reads or writes it |
| 📝 **Find What Writes** | Same, but scoped to write accesses only |
| 📊 **Access Hits Window** | Dedicated results window listing hit instructions with hit counts, disassembly, and thread ID — double-click to jump to the instruction in the Memory Viewer |

### 🖥️ Memory Viewer

A separate resizable window with a split **disassembly + hex dump** layout.

| Feature | Description |
|---------|-------------|
| 📜 **Hex Dump** | 16-byte rows with address, hex, and ASCII columns |
| 🔧 **Capstone Disassembly** | Full x86/x64 disassembly via Capstone 5.0.3 with mnemonic + operands |
| 📍 **Module-Relative Addresses** | All addresses shown as `MAT.exe+85023` when inside a module |
| 🔍 **Go To Address** | Address bar accepts hex (`00400000`) or module-relative (`MAT.exe+85023`) |
| 🔄 **Sync Hex / Disasm** | One-click sync between the two views |
| ✏️ **Inline Hex Editor** | Click any byte and edit **nibble-by-nibble** — type hex chars, Tab/arrow to navigate |
| 🚫 **NOP** | NOP an instruction or an entire hex line (`0x90`) |
| 📝 **Patch Bytes** | Dialog to write arbitrary hex bytes (`90 90 EB 05`) |
| 🛠️ **Assemble Dialog** | Change opcodes by patching hex bytes (shows current instruction for reference) |
| 🎯 **AOB Signature Maker** | Interactive byte grid — click to toggle wildcards, adjustable length, copy-to-clipboard |
| 🖱️ **Context Menu** | Add to table, NOP, Patch, AOB Sig, Assemble, Follow, Copy |
| ⌨️ **Keyboard Navigation** | Arrows, PgUp/PgDn, Home/End, Tab, Ctrl+G for Go-To |

### 🗺️ Region & Module Lists

| View | Columns |
|------|---------|
| 📌 **Memory Regions** | Base Address · Size · Protection · Writable |
| 📦 **Module List** | Module Name · Base Address · Size |

Double-click any region or module to jump straight to it in the Memory Viewer. Right-click modules to copy their base address.

### ⚙️ Settings

| Setting | Description |
|---------|-------------|
| 🔗 **Auto-Attach** | Process name to auto-attach on launch (e.g. `MAT.exe`) |
| 🔤 **Font Size** | Adjustable UI font (6–20 pt), live re-render |
| 🐞 **Debugger Type** | `debugger_type` — selects the debug backend: `0` = None, `1` = VEH, `2` = Windows (default) |
| 💾 **Persistence** | Saved to `miku_settings.ini` |

### ⌨️ Hotkeys

| Hotkey | Action |
|--------|--------|
| `Ctrl+F` | **F**irst / Next Scan |
| `Ctrl+R` | **R**eset Scan |
| `Ctrl+O` | **O**pen Process picker |
| `Ctrl+S` | **S**ave address table |
| `Ctrl+M` | Toggle **M**emory Viewer |
| `Ctrl+G` | Toggle Memory Viewer (**G**o-To) |
| `Esc` | Defocus active text input |

### 🎨 UI / Theming

| Feature | Description |
|---------|-------------|
| 🪟 **Borderless Overlay** | `WS_POPUP` topmost window with custom title bar, draggable, resizable, min/max/close buttons |
| 🌈 **Neon Glow Borders** | Multi-pass glow rendering around the entire window and all dialogs |
| 🎨 **VS2022 Dark Theme** | Blue-accent dark palette (`#1C1C2A` background, `#00A8FF` neon, `#007ACC` accent) |
| ✨ **Rounded Corners** | Windows 11 DWM corner preference + drop shadow |
| 🧩 **Immediate-Mode Widgets** | Custom `Button`, `Checkbox`, `TextInput`, `ComboBox`, `ProgressBar`, `Scrollbar`, `Label` |
| 🖼️ **Double-Buffered** | Flicker-free rendering via `CreateCompatibleDC` + `BitBlt` |
| 🔤 **Consolas Font** | Monospaced rendering for hex, addresses, and disassembly |
| 📐 **Responsive Layout** | Panels reflow and resize dynamically — controls never go off-screen |

---

## 🚀 Usage — Quick Start

```
┌─────────────────────────────────────────────────────┐
│  1. LAUNCH     →  Run MikuWrathEngine.exe           │
│  2. ATTACH     →  Click "Open Process" (or Ctrl+O)  │
│  3. SCAN       →  Pick value type, type a value,    │
│                   click "First Scan" (or Ctrl+F)    │
│  4. NARROW     →  Change the value in-game, come    │
│                   back, click "Next Scan"           │
│  5. FREEZE     →  Double-click a result to add it   │
│                   to the table, tick the freeze box │
│  6. EXPLORE    →  Open Memory Viewer (Ctrl+M) to    │
│                   disassemble, NOP, patch, and      │
│                   build AOB signatures              │
└─────────────────────────────────────────────────────┘
```

### Example: Finding & Freezing Health

1. **Attach** to your game process
2. Set type to **4 Bytes**, enter your current health value, click **First Scan**
3. Go take damage in-game, come back, click **Next Scan** (decreased)
4. Repeat until you have a handful of results
5. **Double-click** the correct address → it drops into the address table
6. Type your desired value, press **Enter**, then tick the **Fz** (freeze) checkbox

### Example: AOB Signature with Wildcards

1. Open **Memory Viewer** → right-click an instruction → **AOB Signature...**
2. Click bytes to toggle them into `??` wildcards
3. Copy the generated pattern (e.g. `48 8B ?? ?? 7F ?? 90`) to your project

---

## 📁 Project Structure

```
MikuWrathEngine/
├── CMakeLists.txt              # Build config — FetchContent pulls Capstone 5.0.3
├── build.bat                   # One-click build script
├── .gitignore
├── README.md                   # You are here
│
└── src/
    ├── main.cpp                # WinMain, WndProc, borderless window setup, DWM
    ├── app.h                   # App class — orchestrates all subsystems
    ├── app.cpp                 # Main UI rendering, scan panel, results, tables, dialogs
    ├── ui.h                    # Immediate-mode UI framework + VS2022 dark theme
    ├── ui.cpp                  # GDI+ widget implementations (Button, Combo, etc.)
    ├── types.h                 # Shared types: ValueType, AddressEntry, clipboard helpers
    ├── settings.h              # Settings struct (auto-attach, font size) + INI persistence
    ├── value_utils.h           # Read/write values as strings (inline helpers)
    ├── process_manager.h       # Process attach, RPM/WPM, region/module enumeration
    ├── process_manager.cpp     # Toolhelp32 + VirtualQueryEx + EnumProcessModulesEx
    ├── scanner.h               # Async memory scanner (std::thread + std::mutex)
    ├── scanner.cpp             # New/Next scan workers, AOB parsing, value comparison
    ├── disassembler.h          # Capstone wrapper (x86/x64 init, disasm, prev-instruction)
    ├── disassembler.cpp        # cs_open / cs_disasm / FindPreviousInstruction
    ├── memory_viewer.h         # Memory Viewer window (hex + disasm + editor)
    ├── memory_viewer.cpp       # Hex dump, inline nibble editor, NOP/patch/assemble/AOB sig
    ├── address_table.h         # Address table (freeze, save/load .mwt)
    ├── address_table.cpp       # MWT2 file format, frozen-value writer, live value updates
    ├── debugger.h              # Debugger interface — breakpoints, DR0-DR3 slots, find accesses/writes
    ├── debugger.cpp            # Windows/VEH debug loop, INT3 + hardware breakpoints, single-step re-arm
    ├── access_hits_window.h    # "Find what accesses/writes" results window
    └── access_hits_window.cpp  # Hit list rendering, per-instruction disasm cache, go-to callback
```

---

## 🧬 Tech Stack

| Technology | Role |
|------------|------|
| **C++20** | Core language — `std::thread`, `std::atomic`, `std::mutex`, structured bindings |
| **Win32 API** | Windowing, message loop, process/memory interaction (`ReadProcessMemory`, `WriteProcessMemory`, `VirtualQueryEx`, `Toolhelp32`) |
| **GDI+** | All rendering — no DirectX, no ImGui, no third-party UI framework |
| **Capstone 5.0.3** | Disassembly engine (x86/x64), fetched via CMake `FetchContent` |
| **DWM** | Window rounded corners + drop shadow (`DwmSetWindowAttribute`, `DwmExtendFrameIntoClientArea`) |
| **CMake 3.16+** | Build system with MSVC (`/W3 /utf-8 /MP`) |
| **PSAPI** | Module enumeration (`EnumProcessModulesEx`, `GetModuleInformation`) |
| **Comdlg32** | File open/save dialogs for `.mwt` tables |

### Linked Libraries

```
gdiplus · dwmapi · gdi32 · user32 · psapi · capstone · Comdlg32
```

---

## 🎨 Theme Palette

| Token | Hex | Usage |
|-------|-----|-------|
| `BG_MAIN` | `#1C1C2A` | Window background |
| `BG_PANEL` | `#222230` | Panel background |
| `BG_CONTROL` | `#2A2A3A` | Control background |
| `BG_SELECTED` | `#264078` | Selected row |
| `BG_TITLE` | `#161624` | Title bar |
| `NEON` | `#00A8FF` | Neon glow borders, caret |
| `ACCENT` | `#007ACC` | VS2022 accent blue |
| `CLR_TEXT` | `#DCDC EC` | Primary text |
| `CLR_GREEN` | `#4EC9B0` | Success / attached |
| `CLR_RED` | `#F44747` | Error / no process |
| `CLR_BLUE` | `#569CD6` | Addresses |
| `CLR_YELLOW` | `#DCDC78` | Results count / wildcards |

---

## 📄 License

```
MIT License

Copyright (c) 2026 SyferX (Afdul)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 👤 Author

<div align="center">

**SyferX (Afdul)**

*Reverse engineer · C++ developer · the same person*

<p>
  <a href="https://github.com/RealSyferX">
    <img alt="GitHub" src="https://img.shields.io/badge/GitHub-@RealSyferX-181717?style=for-the-badge&logo=github&logoColor=white">
  </a>
  <a href="https://wrath.syferx.net">
    <img alt="Website" src="https://img.shields.io/badge/Website-wrath.syferx.net-00A8FF?style=for-the-badge&logo=internet-explorer&logoColor=white">
  </a>
  <a href="https://github.com/RealSyferX/MikuWrathEngine">
    <img alt="Repo" src="https://img.shields.io/badge/Repo-MikuWrathEngine-007ACC?style=for-the-badge&logo=git&logoColor=white">
  </a>
</p>

</div>

---

<div align="center">

<sub>⚡ Built from scratch with pure Win32 GDI+ — no bloat, no frameworks, just code. ⚡</sub>

<sub>If this project helped you, drop a ⭐ on [GitHub](https://github.com/RealSyferX/MikuWrathEngine).</sub>

</div>
