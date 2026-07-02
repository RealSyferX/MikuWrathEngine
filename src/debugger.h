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
    bool bpActive = false;   // true after 0xCC has been written to target
    bool enabled = true;
    char label[128] = {};
};

struct RegisterSnapshot {
    uint64_t rax=0, rbx=0, rcx=0, rdx=0;
    uint64_t rsi=0, rdi=0, rsp=0, rbp=0;
    uint64_t r8=0, r9=0, r10=0, r11=0;
    uint64_t r12=0, r13=0, r14=0, r15=0;
    uint64_t rip=0;
    uint32_t eflags=0;
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
    bool IsHalted() const { return m_halted.load(); }
    RegisterSnapshot GetLastContext() const;
    bool StepInto();
    bool Continue();

    // Wait for debug thread to be ready (after Attach)
    bool WaitForReady(int timeoutMs = 5000);

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

    HANDLE m_readyEvent = nullptr;        // Signaled when debug thread is ready
    HANDLE m_resumeEvent = nullptr;       // Signaled by StepInto/Continue to resume a halted thread
    std::atomic<bool> m_halted{false};    // True when target is stopped at a breakpoint
    RegisterSnapshot m_lastContext;       // Last captured thread context (at halt)
    DWORD m_haltedThreadId = 0;           // Thread that triggered the halt
    std::atomic<bool> m_hwBpDirty{false}; // Flag: breakpoints need re-applying

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
    void SingleStepThread(DWORD tid);
    void CaptureContext(DWORD tid);
};
