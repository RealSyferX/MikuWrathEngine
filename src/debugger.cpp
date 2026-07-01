#include "debugger.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

Debugger::~Debugger() {
    Detach();
}

void Debugger::EnumerateThreads() {
    m_threadIds.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == m_pid) {
                m_threadIds.insert(te.th32ThreadID);
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

int Debugger::FindFreeHwSlot() {
    for (int i = 0; i < 4; i++) {
        if (!m_hwSlotUsed[i]) return i;
    }
    return -1;
}

bool Debugger::SetHwBreakpointInContext(CONTEXT& ctx, int slot, uintptr_t addr, BreakType type, size_t size) {
    if (slot < 0 || slot > 3) return false;

    // Set address register
    switch (slot) {
    case 0: ctx.Dr0 = addr; break;
    case 1: ctx.Dr1 = addr; break;
    case 2: ctx.Dr2 = addr; break;
    case 3: ctx.Dr3 = addr; break;
    }

    // Configure DR7
    // Bits: L0/G0 at 0/1, L1/G1 at 2/3, etc.
    // Condition bits at 16+slot*4: 00=execute, 01=write, 11=read/write
    // Length bits at 18+slot*4: 00=1byte, 01=2bytes, 11=4bytes, 10=8bytes
    DWORD enableBit = 1 << (slot * 2); // Local enable

    DWORD cond = 0;
    switch (type) {
    case BreakType::Execute: cond = 0; break;       // 00
    case BreakType::Write:   cond = 1; break;       // 01
    case BreakType::Access:  cond = 3; break;       // 11
    }

    DWORD len = 0;
    switch (size) {
    case 1: len = 0; break;  // 00
    case 2: len = 1; break;  // 01
    case 4: len = 3; break;  // 11
    case 8: len = 2; break;  // 10
    default: len = 0; break;
    }

    ctx.Dr7 |= enableBit;
    ctx.Dr7 |= (cond << (16 + slot * 4));
    ctx.Dr7 |= (len << (18 + slot * 4));

    return true;
}

bool Debugger::ClearHwBreakpointInContext(CONTEXT& ctx, int slot) {
    if (slot < 0 || slot > 3) return false;

    // Clear local enable
    ctx.Dr7 &= ~(1 << (slot * 2));
    // Clear condition
    ctx.Dr7 &= ~(3 << (16 + slot * 4));
    // Clear length
    ctx.Dr7 &= ~(3 << (18 + slot * 4));

    // Clear address
    switch (slot) {
    case 0: ctx.Dr0 = 0; break;
    case 1: ctx.Dr1 = 0; break;
    case 2: ctx.Dr2 = 0; break;
    case 3: ctx.Dr3 = 0; break;
    }
    return true;
}

void Debugger::ApplyHardwareBreakpointsToThread(DWORD threadId) {
    HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, threadId);
    if (!hThread) return;

    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(hThread, &ctx)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, bp] : m_breakpoints) {
            if (bp.hardware && bp.enabled && bp.hwSlot >= 0) {
                SetHwBreakpointInContext(ctx, bp.hwSlot, bp.address, bp.type, bp.size);
            }
        }
        SetThreadContext(hThread, &ctx);
    }
    CloseHandle(hThread);
}

void Debugger::ApplyHardwareBreakpointsToAll() {
    EnumerateThreads();
    for (DWORD tid : m_threadIds) {
        ApplyHardwareBreakpointsToThread(tid);
    }
}

bool Debugger::Attach() {
    if (!m_pm || !m_pm->IsOpen()) return false;
    if (m_attached.load()) return false;

    m_pid = m_pm->GetPid();

    if (m_type == DebuggerType::Windows || m_type == DebuggerType::VEH) {
        if (!DebugActiveProcess(m_pid)) {
            return false;
        }
        // Don't kill the process on detach
        DebugSetProcessKillOnExit(FALSE);
    }

    m_attached.store(true);
    m_running.store(true);
    m_thread = std::thread(&Debugger::DebugThread, this);
    return true;
}

void Debugger::Detach() {
    if (!m_attached.load()) return;

    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();

    // Clear all hardware breakpoints
    EnumerateThreads();
    for (DWORD tid : m_threadIds) {
        HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
        if (hThread) {
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(hThread, &ctx)) {
                for (int i = 0; i < 4; i++) {
                    ClearHwBreakpointInContext(ctx, i);
                }
                SetThreadContext(hThread, &ctx);
            }
            CloseHandle(hThread);
        }
    }

    // Restore INT3 bytes
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, bp] : m_breakpoints) {
            if (!bp.hardware && bp.originalByte != 0xCC) {
                m_pm->Write(bp.address, &bp.originalByte, 1);
            }
        }
    }

    // Detach debugger
    if (m_type == DebuggerType::Windows || m_type == DebuggerType::VEH) {
        DebugActiveProcessStop(m_pid);
    }

    m_attached.store(false);
}

int Debugger::AddBreakpoint(uintptr_t addr, BreakType type, size_t size, const char* label) {
    std::lock_guard<std::mutex> lock(m_mutex);

    Breakpoint bp;
    bp.address = addr;
    bp.type = type;
    bp.size = size;
    if (label) {
        strncpy(bp.label, label, sizeof(bp.label) - 1);
        bp.label[sizeof(bp.label) - 1] = '\0';
    }

    // Decide: hardware or INT3
    if (type == BreakType::Execute && m_type == DebuggerType::Windows) {
        // INT3 breakpoint
        bp.hardware = false;
        uint8_t orig = 0;
        if (m_pm->Read(addr, &orig, 1)) {
            bp.originalByte = orig;
            uint8_t int3 = 0xCC;
            m_pm->Write(addr, &int3, 1);
        }
    } else {
        // Hardware breakpoint
        int slot = FindFreeHwSlot();
        if (slot < 0) return -1; // No free slots
        bp.hardware = true;
        bp.hwSlot = slot;
        m_hwSlotUsed[slot] = 1;
        ApplyHardwareBreakpointsToAll();
    }

    int id = m_nextBpId++;
    m_breakpoints[id] = bp;
    return id;
}

bool Debugger::RemoveBreakpoint(int id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_breakpoints.find(id);
    if (it == m_breakpoints.end()) return false;

    Breakpoint& bp = it->second;
    if (bp.hardware) {
        m_hwSlotUsed[bp.hwSlot] = 0;
        ApplyHardwareBreakpointsToAll();
    } else {
        // Restore original byte
        if (bp.originalByte != 0xCC) {
            m_pm->Write(bp.address, &bp.originalByte, 1);
        }
    }

    m_breakpoints.erase(it);
    return true;
}

bool Debugger::ClearAllBreakpoints() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, bp] : m_breakpoints) {
        if (bp.hardware) {
            m_hwSlotUsed[bp.hwSlot] = 0;
        } else if (bp.originalByte != 0xCC) {
            m_pm->Write(bp.address, &bp.originalByte, 1);
        }
    }
    m_breakpoints.clear();
    memset(m_hwSlotUsed, 0, sizeof(m_hwSlotUsed));
    ApplyHardwareBreakpointsToAll();
    return true;
}

std::vector<Breakpoint> Debugger::GetBreakpoints() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Breakpoint> result;
    for (auto& [id, bp] : m_breakpoints) {
        result.push_back(bp);
    }
    return result;
}

uintptr_t Debugger::GetInstructionPointer(DWORD threadId) {
    HANDLE hThread = OpenThread(THREAD_GET_CONTEXT, FALSE, threadId);
    if (!hThread) return 0;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_CONTROL;
    uintptr_t ip = 0;
    if (GetThreadContext(hThread, &ctx)) {
#ifdef _WIN64
        ip = ctx.Rip;
#else
        ip = ctx.Eip;
#endif
    }
    CloseHandle(hThread);
    return ip;
}

void Debugger::RecordAccessHit(DWORD threadId, uintptr_t ip) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Check if we already have this instruction
    for (auto& hit : m_accessHits) {
        if (hit.instruction == ip) {
            hit.count++;
            return;
        }
    }
    AccessHit hit;
    hit.instruction = ip;
    hit.threadId = threadId;
    hit.count = 1;
    m_accessHits.push_back(hit);
}

bool Debugger::StartFindAccesses(uintptr_t addr, size_t size) {
    if (m_finding.load()) StopFind();
    if (!m_attached.load()) {
        if (!Attach()) return false;
    }
    m_finding.store(true);
    m_findAddr = addr;
    m_findType = BreakType::Access;
    m_findSize = size;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accessHits.clear();
    }
    m_findBpId = AddBreakpoint(addr, BreakType::Access, size, "FindAccesses");
    return m_findBpId >= 0;
}

bool Debugger::StartFindWrites(uintptr_t addr, size_t size) {
    if (m_finding.load()) StopFind();
    if (!m_attached.load()) {
        if (!Attach()) return false;
    }
    m_finding.store(true);
    m_findAddr = addr;
    m_findType = BreakType::Write;
    m_findSize = size;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accessHits.clear();
    }
    m_findBpId = AddBreakpoint(addr, BreakType::Write, size, "FindWrites");
    return m_findBpId >= 0;
}

void Debugger::StopFind() {
    m_finding.store(false);
    if (m_findBpId >= 0) {
        RemoveBreakpoint(m_findBpId);
        m_findBpId = -1;
    }
}

std::vector<AccessHit> Debugger::GetAccessHits() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_accessHits;
}

void Debugger::DebugThread() {
    DEBUG_EVENT de = {};

    while (m_running.load()) {
        if (!WaitForDebugEvent(&de, 100)) {
            continue;
        }

        DWORD continueStatus = DBG_CONTINUE;

        switch (de.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD& er = de.u.Exception.ExceptionRecord;

            if (er.ExceptionCode == EXCEPTION_BREAKPOINT || er.ExceptionCode == EXCEPTION_SINGLE_STEP) {
                // Get instruction pointer
                uintptr_t ip = (uintptr_t)er.ExceptionAddress;
                DWORD tid = de.dwThreadId;

                if (m_finding.load()) {
                    // This is a find-access/write hit
                    RecordAccessHit(tid, ip);
                    // Just continue — hardware breakpoint auto-resets
                    continueStatus = DBG_CONTINUE;
                } else {
                    // Regular breakpoint hit
                    // For INT3: the IP is at the breakpoint + 1 (after 0xCC)
                    // We need to restore the original byte, single-step, then re-set
                    // For hardware: the IP is at the instruction that triggered

                    // For now, just continue
                    continueStatus = DBG_CONTINUE;
                }
            } else {
                // Other exception — let the program handle it
                continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        }
        case CREATE_THREAD_DEBUG_EVENT:
            m_threadIds.insert(de.dwThreadId);
            ApplyHardwareBreakpointsToThread(de.dwThreadId);
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            m_threadIds.erase(de.dwThreadId);
            break;
        case CREATE_PROCESS_DEBUG_EVENT:
            // Initial setup
            EnumerateThreads();
            ApplyHardwareBreakpointsToAll();
            // Close the handle from the create-process event to avoid leaks
            if (de.u.CreateProcessInfo.hProcess) {
                CloseHandle(de.u.CreateProcessInfo.hProcess);
            }
            if (de.u.CreateProcessInfo.hThread) {
                CloseHandle(de.u.CreateProcessInfo.hThread);
            }
            if (de.u.CreateProcessInfo.hFile) {
                CloseHandle(de.u.CreateProcessInfo.hFile);
            }
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            m_running.store(false);
            break;
        }

        ContinueDebugEvent(de.dwProcessId, de.dwThreadId, continueStatus);
    }
}
