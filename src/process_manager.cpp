#include "process_manager.h"
#include "parse_utils.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#pragma comment(lib, "psapi.lib")

std::vector<ProcessInfo> ProcessManager::EnumerateProcesses() {
    std::vector<ProcessInfo> procs;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return procs;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4) continue;

            ProcessInfo info;
            info.pid = pe.th32ProcessID;
            char nameBuf[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, nameBuf, MAX_PATH, nullptr, nullptr);
            info.name = nameBuf;
            procs.push_back(info);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::sort(procs.begin(), procs.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return procs;
}

bool ProcessManager::OpenTarget(DWORD pid) {
    CloseTarget();

    m_hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE, pid);
    if (m_hProcess) {
        m_writable = true;
    } else {
        m_hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!m_hProcess) return false;
        m_writable = false;
    }

    m_pid = pid;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == pid) {
                    char nameBuf[MAX_PATH];
                    WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, nameBuf, MAX_PATH, nullptr, nullptr);
                    m_processName = nameBuf;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

#if defined(_WIN64)
    BOOL isWow64 = FALSE;
    IsWow64Process(m_hProcess, &isWow64);
    m_is64Bit = !isWow64;
#else
    m_is64Bit = false;
#endif

    m_modulesDirty = true;
    return true;
}

void ProcessManager::CloseTarget() {
    // NOTE: This closes m_hProcess with no synchronization against background
    // threads. Read/ReadPartial/Write and the scanner worker consume the raw
    // handle continuously, so the CALLER must stop all handle-consuming
    // background work (scanner via Scanner::Reset(), debugger via
    // Debugger::Detach()) BEFORE calling this, otherwise the handle can be torn
    // out from under an in-flight read (torn read / handle-reuse race).
    if (m_hProcess) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }
    m_pid = 0;
    m_processName.clear();
    m_is64Bit = false;
    m_writable = false;
    m_modulesDirty = true;
    m_cachedModules.clear();
}

bool ProcessManager::Read(uintptr_t addr, void* buf, size_t size) const {
    if (!m_hProcess) return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(m_hProcess, (LPCVOID)addr, buf, size, &bytesRead) && bytesRead == size;
}

size_t ProcessManager::ReadPartial(uintptr_t addr, void* buf, size_t size) const {
    if (!m_hProcess) return 0;
    SIZE_T bytesRead = 0;
    ReadProcessMemory(m_hProcess, (LPCVOID)addr, buf, size, &bytesRead);
    return (size_t)bytesRead;
}

bool ProcessManager::Write(uintptr_t addr, const void* buf, size_t size) const {
    if (!m_hProcess) return false;
    SIZE_T written = 0;
    return WriteProcessMemory(m_hProcess, (LPVOID)addr, buf, size, &written) && written == size;
}

std::vector<MemoryRegion> ProcessManager::EnumerateRegions(bool writableOnly) const {
    std::vector<MemoryRegion> regions;
    if (!m_hProcess) return regions;

    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQueryEx(m_hProcess, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT) {
            DWORD prot = mbi.Protect;
            bool readable = !(prot & PAGE_NOACCESS) && !(prot & PAGE_GUARD);
            bool writable = (prot & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                            PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;

            if (readable && (!writableOnly || writable)) {
                MemoryRegion region;
                region.base = (uintptr_t)mbi.BaseAddress;
                region.size = mbi.RegionSize;
                region.protect = prot;
                region.writable = writable;
                regions.push_back(region);
            }
        }

        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }

    return regions;
}

std::string ProcessManager::GetProcessPath() const {
    if (!m_hProcess) return "";
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(m_hProcess, 0, path, &len)) {
        char buf[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, buf, MAX_PATH, nullptr, nullptr);
        return buf;
    }
    return "";
}

bool ProcessManager::EnumModules(std::vector<HMODULE>& out) const {
    out.clear();
    if (!m_hProcess) return false;

    // First query for the required byte count. Then size the buffer and fill it.
    // The module list can grow between the two calls (modules loaded on another
    // thread), so retry until the buffer is large enough to hold everything.
    DWORD needed = 0;
    if (!EnumProcessModulesEx(m_hProcess, nullptr, 0, &needed, LIST_MODULES_ALL))
        return false;

    for (int attempt = 0; attempt < 8 && needed > 0; attempt++) {
        out.resize(needed / sizeof(HMODULE));
        DWORD bufSize = (DWORD)(out.size() * sizeof(HMODULE));
        DWORD got = 0;
        if (!EnumProcessModulesEx(m_hProcess, out.data(), bufSize, &got, LIST_MODULES_ALL)) {
            out.clear();
            return false;
        }
        if (got <= bufSize) {
            // Everything fit; trim to the actual number of handles returned.
            out.resize(got / sizeof(HMODULE));
            return true;
        }
        // The list grew; loop again with the larger required size.
        needed = got;
    }

    out.clear();
    return false;
}

uintptr_t ProcessManager::GetModuleBase(const char* moduleName) const {
    if (!m_hProcess) return 0;

    // Reuse the same lazy cache that FormatAddress maintains so manual-add and
    // Go-To operations don't trigger a full EnumProcessModulesEx sweep plus a
    // GetModuleBaseNameW call per module on every parse.
    if (m_modulesDirty) {
        m_cachedModules = EnumerateModules();
        m_modulesDirty = false;
    }

    for (auto& m : m_cachedModules) {
        // A null moduleName means "any module"; preserve the original behavior
        // of returning the first enumerated module's base.
        if (!moduleName || _stricmp(m.name.c_str(), moduleName) == 0) {
            return m.base;
        }
    }
    return 0;
}

std::vector<ProcessManager::ModuleInfo> ProcessManager::EnumerateModules() const {
    std::vector<ModuleInfo> mods;
    if (!m_hProcess) return mods;

    std::vector<HMODULE> hMods;
    if (!EnumModules(hMods)) return mods;

    for (HMODULE mod : hMods) {
        wchar_t wname[MAX_PATH];
        if (GetModuleBaseNameW(m_hProcess, mod, wname, MAX_PATH)) {
            MODULEINFO mi;
            if (GetModuleInformation(m_hProcess, mod, &mi, sizeof(mi))) {
                ModuleInfo info;
                info.base = (uintptr_t)mi.lpBaseOfDll;
                info.size = mi.SizeOfImage;
                char nameBuf[MAX_PATH];
                WideCharToMultiByte(CP_UTF8, 0, wname, -1, nameBuf, MAX_PATH, nullptr, nullptr);
                info.name = nameBuf;
                mods.push_back(info);
            }
        }
    }
    return mods;
}

std::string ProcessManager::FormatAddress(uintptr_t addr) const {
    if (m_modulesDirty) {
        m_cachedModules = EnumerateModules();
        m_modulesDirty = false;
    }
    for (auto& m : m_cachedModules) {
        if (addr >= m.base && addr < m.base + m.size) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s+%llX", m.name.c_str(),
                     (unsigned long long)(addr - m.base));
            return buf;
        }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)addr);
    return buf;
}

uintptr_t ProcessManager::ParseAddressString(const std::string& str) const {
    // The string-trimming and offset math live in a pure helper so they can be
    // unit-tested without a live process; only the module-base lookup here
    // depends on an open target.
    std::string modName;
    uintptr_t offset = 0;
    if (SplitModuleOffset(str, modName, offset)) {
        uintptr_t base = GetModuleBase(modName.c_str());
        if (offset > UINTPTR_MAX - base) return UINTPTR_MAX;  // saturate on wrap
        return base + offset;
    }
    // Plain hex address (accept optional 0x prefix)
    return ParsePlainHexAddress(str);
}
