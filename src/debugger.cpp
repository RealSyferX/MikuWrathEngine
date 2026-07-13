#include "debugger.h"
#include "disassembler.h"
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
    HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
    if (!hThread) return;

    DWORD suspendCount = SuspendThread(hThread);
    if (suspendCount == (DWORD)-1) {
        CloseHandle(hThread);
        return;
    }

    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(hThread, &ctx)) {
        // Clear all slots first
        for (int i = 0; i < 4; i++) {
            ClearHwBreakpointInContext(ctx, i);
        }
        // Set active breakpoints
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& [id, bp] : m_breakpoints) {
                if (bp.hardware && bp.enabled && bp.hwSlot >= 0) {
                    SetHwBreakpointInContext(ctx, bp.hwSlot, bp.address, bp.type, bp.size);
                }
            }
        }
        SetThreadContext(hThread, &ctx);
    }

    ResumeThread(hThread);
    CloseHandle(hThread);
}

void Debugger::ApplyHardwareBreakpointsToAll() {
    EnumerateThreads();
    for (DWORD tid : m_threadIds) {
        HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!hThread) continue;

        // Suspend the thread before modifying context
        DWORD suspendCount = SuspendThread(hThread);
        if (suspendCount == (DWORD)-1) {
            CloseHandle(hThread);
            continue;
        }

        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(hThread, &ctx)) {
            // Clear all hardware breakpoints
            for (int i = 0; i < 4; i++) {
                ClearHwBreakpointInContext(ctx, i);
            }
            // Set active breakpoints
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& [id, bp] : m_breakpoints) {
                    if (bp.hardware && bp.enabled && bp.hwSlot >= 0) {
                        SetHwBreakpointInContext(ctx, bp.hwSlot, bp.address, bp.type, bp.size);
                    }
                }
            }
            SetThreadContext(hThread, &ctx);
        }

        ResumeThread(hThread);
        CloseHandle(hThread);
    }
}

bool Debugger::Attach() {
    if (!m_pm || !m_pm->IsOpen()) return false;
    if (m_attached.load()) return false;
    // A no-op attach would spin a debug thread that busy-loops on
    // WaitForDebugEvent forever and block for the full m_readyEvent timeout.
    if (m_type == DebuggerType::None) return false;

    m_pid = m_pm->GetPid();

    if (m_type == DebuggerType::Windows || m_type == DebuggerType::VEH) {
        if (!DebugActiveProcess(m_pid)) {
            return false;
        }
        // Don't kill the process on detach
        DebugSetProcessKillOnExit(FALSE);
    }

    m_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_resumeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual-reset

    m_attached.store(true);
    m_running.store(true);
    m_thread = std::thread(&Debugger::DebugThread, this);

    // Wait for debug thread to receive CREATE_PROCESS_DEBUG_EVENT
    if (m_readyEvent) {
        WaitForSingleObject(m_readyEvent, 5000);
    }

    return true;
}

void Debugger::Detach() {
    if (!m_attached.load()) return;

    // Clear halted state early so observers see the target is no longer
    // stopped at a breakpoint.
    m_halted.store(false);

    m_running.store(false);
    // Unconditionally wake the debug thread. It may be parked on the wait on
    // m_resumeEvent after halting at a breakpoint. Relying on m_halted.load()
    // here races the debug thread, which stores m_halted=true then blocks: if
    // that store isn't yet visible, Detach would skip SetEvent and join()
    // would hang forever. m_resumeEvent is manual-reset, so signalling when no
    // one waits is harmless — the debug thread observes m_running==false on its
    // next loop iteration and exits cleanly.
    if (m_resumeEvent) SetEvent(m_resumeEvent);
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
            if (!bp.hardware && bp.bpActive) {
                m_pm->Write(bp.address, &bp.originalByte, 1);
            }
        }
    }

    // Detach debugger
    if (m_type == DebuggerType::Windows || m_type == DebuggerType::VEH) {
        DebugActiveProcessStop(m_pid);
    }

    m_attached.store(false);

    if (m_readyEvent) {
        CloseHandle(m_readyEvent);
        m_readyEvent = nullptr;
    }
    if (m_resumeEvent) {
        CloseHandle(m_resumeEvent);
        m_resumeEvent = nullptr;
    }
}

bool Debugger::WaitForReady(int timeoutMs) {
    if (!m_readyEvent) return false;
    return WaitForSingleObject(m_readyEvent, timeoutMs) == WAIT_OBJECT_0;
}

int Debugger::AddBreakpoint(uintptr_t addr, BreakType type, size_t size, const char* label) {
    int id;
    bool needApply = false;
    {
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
                if (m_pm->Write(addr, &int3, 1)) {
                    bp.bpActive = true;
                }
            }
        } else {
            // Hardware breakpoint
            int slot = FindFreeHwSlot();
            if (slot < 0) return -1; // No free slots
            bp.hardware = true;
            bp.hwSlot = slot;
            m_hwSlotUsed[slot] = 1;
            if (m_attached.load()) {
                m_hwBpDirty.store(true);
            } else {
                // Defer the re-apply until after releasing m_mutex —
                // ApplyHardwareBreakpointsToAll() locks m_mutex itself and
                // m_mutex is non-recursive, so calling it here would deadlock.
                needApply = true;
            }
        }

        id = m_nextBpId++;
        m_breakpoints[id] = bp;
    }

    // Lock released. Apply now (if detached) so the new breakpoint takes effect.
    if (needApply) {
        ApplyHardwareBreakpointsToAll();
    }
    return id;
}

bool Debugger::RemoveBreakpoint(int id) {
    bool needApply = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_breakpoints.find(id);
        if (it == m_breakpoints.end()) return false;

        Breakpoint& bp = it->second;
        if (bp.hardware) {
            m_hwSlotUsed[bp.hwSlot] = 0;
            if (m_attached.load()) {
                m_hwBpDirty.store(true);
            } else {
                // Defer the re-apply until after releasing m_mutex —
                // ApplyHardwareBreakpointsToAll() locks m_mutex itself and
                // m_mutex is non-recursive, so calling it here would deadlock.
                needApply = true;
            }
        } else {
            // Restore original byte
            if (bp.bpActive) {
                m_pm->Write(bp.address, &bp.originalByte, 1);
            }
        }

        m_breakpoints.erase(it);
    }

    // Lock released. Apply now (if detached) so the removal takes effect.
    if (needApply) {
        ApplyHardwareBreakpointsToAll();
    }
    return true;
}

bool Debugger::ClearAllBreakpoints() {
    bool needApply = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, bp] : m_breakpoints) {
            if (bp.hardware) {
                m_hwSlotUsed[bp.hwSlot] = 0;
            } else if (bp.bpActive) {
                m_pm->Write(bp.address, &bp.originalByte, 1);
            }
        }
        m_breakpoints.clear();
        memset(m_hwSlotUsed, 0, sizeof(m_hwSlotUsed));
        if (m_attached.load()) {
            m_hwBpDirty.store(true);
        } else {
            // Defer the re-apply until after releasing m_mutex —
            // ApplyHardwareBreakpointsToAll() locks m_mutex itself and
            // m_mutex is non-recursive, so calling it here would deadlock.
            needApply = true;
        }
    }

    // Lock released. Apply now (if detached) so the cleared state takes effect.
    if (needApply) {
        ApplyHardwareBreakpointsToAll();
    }
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

void Debugger::SingleStepThread(DWORD tid) {
    HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!hThread) return;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(hThread, &ctx)) {
        ctx.EFlags |= 0x100; // Set trap flag
        SetThreadContext(hThread, &ctx);
    }
    CloseHandle(hThread);
}

void Debugger::CaptureContext(DWORD tid) {
    HANDLE hThread = OpenThread(THREAD_GET_CONTEXT, FALSE, tid);
    if (!hThread) return;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(hThread, &ctx)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastContext.rax = ctx.Rax;
        m_lastContext.rbx = ctx.Rbx;
        m_lastContext.rcx = ctx.Rcx;
        m_lastContext.rdx = ctx.Rdx;
        m_lastContext.rsi = ctx.Rsi;
        m_lastContext.rdi = ctx.Rdi;
        m_lastContext.rsp = ctx.Rsp;
        m_lastContext.rbp = ctx.Rbp;
        m_lastContext.r8  = ctx.R8;
        m_lastContext.r9  = ctx.R9;
        m_lastContext.r10 = ctx.R10;
        m_lastContext.r11 = ctx.R11;
        m_lastContext.r12 = ctx.R12;
        m_lastContext.r13 = ctx.R13;
        m_lastContext.r14 = ctx.R14;
        m_lastContext.r15 = ctx.R15;
        m_lastContext.rip = ctx.Rip;
        m_lastContext.eflags = (uint32_t)ctx.EFlags;
        m_haltedThreadId = tid;
    }
    CloseHandle(hThread);
}

bool Debugger::StepInto() {
    if (!m_halted.load()) return false;
    // Set trap flag on halted thread so it executes exactly one
    // instruction before raising EXCEPTION_SINGLE_STEP again.
    DWORD tid;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tid = m_haltedThreadId;
    }
    HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (hThread) {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(hThread, &ctx)) {
            ctx.EFlags |= 0x100; // Trap flag for single step
            SetThreadContext(hThread, &ctx);
        }
        CloseHandle(hThread);
    }
    m_halted.store(false);
    // Mark that a single-step halt is requested so the EXCEPTION_SINGLE_STEP
    // handler knows to halt (rather than just re-arming INT3 and continuing).
    m_stepRequested.store(true);
    // Signal debug thread to continue
    if (m_resumeEvent) SetEvent(m_resumeEvent);
    return true;
}

bool Debugger::StepOver() {
    if (!m_halted.load()) return false;

    // Get current RIP
    uintptr_t rip;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rip = m_lastContext.rip;
    }

    // Read and disassemble the current instruction
    bool isCall = false;
    size_t instSize = 0;
    if (m_disasm && m_disasm->IsInitialized() && m_pm) {
        uint8_t buf[16];
        if (m_pm->Read(rip, buf, sizeof(buf))) {
            auto insts = m_disasm->Disassemble(rip, buf, sizeof(buf), 1);
            if (!insts.empty()) {
                instSize = insts[0].size;
                // Check if it's a CALL instruction
                if (strncmp(insts[0].mnemonic, "call", 4) == 0) {
                    isCall = true;
                }
            }
        }
    }

    if (isCall && instSize > 0) {
        // Set temporary breakpoint at instruction after the CALL
        m_tempBpId.store(AddBreakpoint(rip + instSize, BreakType::Execute, 1, "__stepover"));
        // Continue — the temp BP will halt us when the CALL returns
        m_stepRequested.store(false);
        m_halted.store(false);
        if (m_resumeEvent) SetEvent(m_resumeEvent);
    } else {
        // Not a CALL — just step into
        return StepInto();
    }
    return true;
}

bool Debugger::Continue() {
    if (!m_halted.load()) return false;
    m_halted.store(false);
    // A plain continue must not trigger a single-step halt.
    m_stepRequested.store(false);
    if (m_resumeEvent) SetEvent(m_resumeEvent);
    return true;
}

RegisterSnapshot Debugger::GetLastContext() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastContext;
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
            // Even when no event, check for dirty breakpoints
            if (m_hwBpDirty.load()) {
                m_hwBpDirty.store(false);
                ApplyHardwareBreakpointsToAll();
            }
            continue;
        }

        DWORD continueStatus = DBG_CONTINUE;

        switch (de.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD& er = de.u.Exception.ExceptionRecord;
            DWORD tid = de.dwThreadId;

            if (er.ExceptionCode == EXCEPTION_BREAKPOINT) {
                // INT3 breakpoint hit.
                // After 0xCC executes the CPU advances IP past it, so the
                // breakpoint address is ExceptionAddress - 1.
                uintptr_t ip = (uintptr_t)er.ExceptionAddress;
                uintptr_t bpAddr = ip - 1;

                bool found = false;
                int foundId = -1;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (auto& [id, bp] : m_breakpoints) {
                        if (!bp.hardware && bp.enabled && bp.address == bpAddr) {
                            // Restore the original byte so the single-step
                            // does not immediately re-hit the 0xCC.
                            m_pm->Write(bp.address, &bp.originalByte, 1);
                            found = true;
                            foundId = id;
                            break;
                        }
                    }
                    if (found) {
                        // Remember this thread needs 0xCC re-patched after
                        // the single-step completes.
                        m_singleStepping.insert(tid);
                    }
                }

                // Check if this is a temporary step-over breakpoint
                int tmp = m_tempBpId.load();
                if (foundId == tmp && tmp != -1) {
                    RemoveBreakpoint(tmp);
                    m_tempBpId.store(-1);
                }

                if (found) {
                    // Arm the trap flag so the next instruction raises
                    // EXCEPTION_SINGLE_STEP, at which point we re-patch 0xCC.
                    SingleStepThread(tid);
                }
                // Whether or not it was our breakpoint (the system attach
                // breakpoint also arrives here), swallow it.
                continueStatus = DBG_CONTINUE;

                // If this was our breakpoint and we're not in find mode,
                // halt and wait for the UI to Step/Continue.
                if (found && !m_finding.load()) {
                    CaptureContext(tid);
                    m_halted.store(true);

                    // Continue this event so the thread is no longer
                    // suspended by the debug subsystem, then wait for
                    // StepInto()/Continue() to signal m_resumeEvent.
                    // The trap flag armed above causes the target to
                    // single-step and re-freeze after one instruction.
                    ContinueDebugEvent(de.dwProcessId, de.dwThreadId, continueStatus);

                    if (m_resumeEvent) {
                        // Poll on a short timeout instead of blocking forever
                        // so a lost/missed signal can never park this thread
                        // permanently. On a real resume the event (manual-reset)
                        // is already signalled, so the wait returns immediately;
                        // we ResetEvent and proceed exactly as before. On detach
                        // m_running is cleared, so we bail out to the loop head.
                        while (m_running.load()) {
                            if (WaitForSingleObject(m_resumeEvent, 200) == WAIT_OBJECT_0) {
                                ResetEvent(m_resumeEvent);
                                break;
                            }
                        }
                    }
                    continue; // Skip the end-of-loop ContinueDebugEvent
                }
            } else if (er.ExceptionCode == EXCEPTION_SINGLE_STEP) {
                uintptr_t ip = (uintptr_t)er.ExceptionAddress;
                bool wasStepping = false;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_singleStepping.find(tid);
                    if (it != m_singleStepping.end()) {
                        // Completed the single-step over a restored INT3.
                        m_singleStepping.erase(it);
                        wasStepping = true;
                        // Re-patch 0xCC for all enabled non-hardware
                        // breakpoints. The one we stepped over is included;
                        // the others already hold 0xCC so this is a no-op for
                        // them. Breakpoints that were removed/disabled since
                        // the hit are naturally skipped.
                        uint8_t int3 = 0xCC;
                        for (auto& [id, bp] : m_breakpoints) {
                            if (!bp.hardware && bp.enabled) {
                                m_pm->Write(bp.address, &int3, 1);
                            }
                        }
                    }
                }

                if (wasStepping) {
                    // INT3 re-arm dance just completed. If the user asked for
                    // a StepInto, halt here instead of running away freely.
                    if (m_stepRequested.load()) {
                        m_stepRequested.store(false);
                        CaptureContext(tid);
                        m_halted.store(true);

                        ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_CONTINUE);

                        if (m_resumeEvent) {
                            // Poll on a short timeout instead of blocking
                            // forever so a lost/missed signal can never park
                            // this thread permanently. Behavior on a real
                            // resume is identical (ResetEvent + proceed); on
                            // detach m_running is cleared and we bail out.
                            while (m_running.load()) {
                                if (WaitForSingleObject(m_resumeEvent, 200) == WAIT_OBJECT_0) {
                                    ResetEvent(m_resumeEvent);
                                    break;
                                }
                            }
                        }
                        continue; // Skip the end-of-loop ContinueDebugEvent
                    }
                    continueStatus = DBG_CONTINUE;
                } else if (m_finding.load()) {
                    // Hardware breakpoint hit while finding accesses/writes.
                    RecordAccessHit(tid, ip);
                    continueStatus = DBG_CONTINUE;
                } else {
                    // Regular hardware breakpoint hit — halt and wait
                    // for StepInto()/Continue() from the UI.
                    continueStatus = DBG_CONTINUE;
                    CaptureContext(tid);
                    m_halted.store(true);

                    ContinueDebugEvent(de.dwProcessId, de.dwThreadId, continueStatus);

                    if (m_resumeEvent) {
                        // Poll on a short timeout instead of blocking forever
                        // so a lost/missed signal can never park this thread
                        // permanently. Behavior on a real resume is identical
                        // (ResetEvent + proceed); on detach m_running is
                        // cleared and we bail out to the loop head.
                        while (m_running.load()) {
                            if (WaitForSingleObject(m_resumeEvent, 200) == WAIT_OBJECT_0) {
                                ResetEvent(m_resumeEvent);
                                break;
                            }
                        }
                    }
                    continue; // Skip the end-of-loop ContinueDebugEvent
                }
            } else {
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
            // Signal that we're ready
            if (m_readyEvent) SetEvent(m_readyEvent);
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            m_running.store(false);
            break;
        }

        // Check for dirty breakpoints (added/removed since last event)
        if (m_hwBpDirty.load()) {
            m_hwBpDirty.store(false);
            ApplyHardwareBreakpointsToAll();
        }

        ContinueDebugEvent(de.dwProcessId, de.dwThreadId, continueStatus);
    }
}
