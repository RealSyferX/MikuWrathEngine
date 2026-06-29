#include "process_manager.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>

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
    if (!m_hProcess) {
        m_hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!m_hProcess) return false;
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

    return true;
}

void ProcessManager::CloseTarget() {
    if (m_hProcess) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }
    m_pid = 0;
    m_processName.clear();
    m_is64Bit = false;
}

bool ProcessManager::Read(uintptr_t addr, void* buf, size_t size) const {
    if (!m_hProcess) return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(m_hProcess, (LPCVOID)addr, buf, size, &bytesRead) && bytesRead == size;
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

uintptr_t ProcessManager::GetModuleBase(const char* moduleName) const {
    if (!m_hProcess) return 0;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(m_hProcess, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
        return 0;
    int count = needed / sizeof(HMODULE);
    for (int i = 0; i < count; i++) {
        wchar_t wname[MAX_PATH];
        if (GetModuleBaseNameW(m_hProcess, mods[i], wname, MAX_PATH)) {
            char nameBuf[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, nameBuf, MAX_PATH, nullptr, nullptr);
            if (!moduleName || _stricmp(nameBuf, moduleName) == 0) {
                MODULEINFO mi;
                if (GetModuleInformation(m_hProcess, mods[i], &mi, sizeof(mi))) {
                    return (uintptr_t)mi.lpBaseOfDll;
                }
            }
        }
    }
    return 0;
}

std::vector<ProcessManager::ModuleInfo> ProcessManager::EnumerateModules() const {
    std::vector<ModuleInfo> mods;
    if (!m_hProcess) return mods;

    HMODULE hMods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(m_hProcess, hMods, sizeof(hMods), &needed, LIST_MODULES_ALL))
        return mods;

    int count = needed / sizeof(HMODULE);
    for (int i = 0; i < count; i++) {
        wchar_t wname[MAX_PATH];
        if (GetModuleBaseNameW(m_hProcess, hMods[i], wname, MAX_PATH)) {
            MODULEINFO mi;
            if (GetModuleInformation(m_hProcess, hMods[i], &mi, sizeof(mi))) {
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
