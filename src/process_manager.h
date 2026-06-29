#pragma once
#include "types.h"
#include <vector>
#include <string>

class ProcessManager {
public:
    std::vector<ProcessInfo> EnumerateProcesses();
    bool OpenTarget(DWORD pid);
    void CloseTarget();

    bool IsOpen() const { return m_hProcess != nullptr; }
    HANDLE GetHandle() const { return m_hProcess; }
    DWORD GetPid() const { return m_pid; }
    const std::string& GetName() const { return m_processName; }
    bool Is64Bit() const { return m_is64Bit; }

    bool Read(uintptr_t addr, void* buf, size_t size) const;
    bool Write(uintptr_t addr, const void* buf, size_t size) const;

    std::vector<MemoryRegion> EnumerateRegions(bool writableOnly = false) const;

    std::string GetProcessPath() const;
    uintptr_t GetModuleBase(const char* moduleName = nullptr) const;

    struct ModuleInfo {
        uintptr_t base = 0;
        size_t size = 0;
        std::string name;
    };
    std::vector<ModuleInfo> EnumerateModules() const;

    // Module-relative address formatting/parsing
    std::string FormatAddress(uintptr_t addr) const;
    uintptr_t ParseAddressString(const std::string& str) const;

private:
    HANDLE m_hProcess = nullptr;
    DWORD m_pid = 0;
    std::string m_processName;
    bool m_is64Bit = false;

    // Lazy module cache for FormatAddress (avoids re-enumerating on every call)
    mutable std::vector<ModuleInfo> m_cachedModules;
    mutable bool m_modulesDirty = true;
};
