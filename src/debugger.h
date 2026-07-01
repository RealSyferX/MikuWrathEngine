#pragma once
#include "types.h"
#include "process_manager.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <map>
#include <vector>
#include <set>

enum class DebuggerType : int {
    None = 0,
    VEH,        // Vectored Exception Handler — hardware breakpoints only
    Windows     // Windows Debug API — INT3 + hardware breakpoints
};

enum class BreakType : int {
    Execute = 0,  // Break on execution
    Write,        // Break on write
    Access        // Break on read or write
};

struct AccessHit {
    uintptr_t instruction = 0;  // Address of instruction that triggered
    DWORD threadId = 0;
    int count = 0;
    char mnemonic[64] = {};  // Disassembled instruction (optional)
};

struct Breakpoint {
    uintptr_t address = 0;
    BreakType type = BreakType::Execute;
    size_t size = 1;
    bool hardware = false;    // true=hardware, false=INT3
    int hwSlot = -1;          // DR0-DR3 slot
    uint8_t originalByte = 0; // For INT3: original byte before 0xCC
    bool enabled = true;
    char label[128] = {};
};

class Debugger {
public:
    Debugger() = default;
    ~Debugger();

    void SetProcessManager(ProcessManager* pm) { m_pm = pm; }
    void SetType(DebuggerType type) { m_type = type; }
    DebuggerType GetType() const { return m_type; }

    bool Attach();
    void Detach();
    bool IsAttached() const { return m_attached.load(); }
    bool IsRunning() const { return m_running.load(); }

    // Breakpoints
    int AddBreakpoint(uintptr_t addr, BreakType type = BreakType::Execute, size_t size = 1, const char* label = "");
    bool RemoveBreakpoint(int id);
    bool ClearAllBreakpoints();
    std::vector<Breakpoint> GetBreakpoints() const;

    // Find what accesses/writes
    bool StartFindAccesses(uintptr_t addr, size_t size = 4);
    bool StartFindWrites(uintptr_t addr, size_t size = 4);
    void StopFind();
    bool IsFinding() const { return m_finding.load(); }
    std::vector<AccessHit> GetAccessHits() const;

private:
    ProcessManager* m_pm = nullptr;
    DWORD m_pid = 0;
    DebuggerType m_type = DebuggerType::Windows;

    std::atomic<bool> m_attached{false};
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    mutable std::mutex m_mutex;

    // Thread tracking
    std::set<DWORD> m_threadIds;

    // Breakpoints
    std::map<int, Breakpoint> m_breakpoints;
    int m_nextBpId = 1;

    // Hardware breakpoint slots (DR0-DR3)
    int m_hwSlotUsed[4] = {0, 0, 0, 0};

    // Find what accesses/writes
    std::atomic<bool> m_finding{false};
    uintptr_t m_findAddr = 0;
    BreakType m_findType = BreakType::Access;
    size_t m_findSize = 4;
    int m_findBpId = -1;
    std::vector<AccessHit> m_accessHits;

    // Single-step state (for INT3 re-set)
    std::set<DWORD> m_singleStepping; // thread IDs that need re-set after step

    void DebugThread();
    void ApplyHardwareBreakpointsToThread(DWORD threadId);
    void ApplyHardwareBreakpointsToAll();
    int FindFreeHwSlot();
    bool SetHwBreakpointInContext(CONTEXT& ctx, int slot, uintptr_t addr, BreakType type, size_t size);
    bool ClearHwBreakpointInContext(CONTEXT& ctx, int slot);

    void EnumerateThreads();
    uintptr_t GetInstructionPointer(DWORD threadId);
    void RecordAccessHit(DWORD threadId, uintptr_t ip);
};
