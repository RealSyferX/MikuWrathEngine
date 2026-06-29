#pragma once
#include "types.h"
#include <capstone/capstone.h>
#include <vector>

class ProcessManager;

class Disassembler {
public:
    Disassembler() = default;
    ~Disassembler();

    bool Init(bool x64);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }
    bool IsX64() const { return m_x64; }

    std::vector<DisasmInstruction> Disassemble(uint64_t address,
                                                const uint8_t* data,
                                                size_t size,
                                                size_t maxCount = 256) const;

    // Try to find the instruction that starts just before 'address'
    // Returns the start address of that instruction, or 'address' on failure
    uintptr_t FindPreviousInstruction(uintptr_t address,
                                       const ProcessManager* pm,
                                       size_t lookback = 128) const;

private:
    csh m_handle = 0;
    bool m_initialized = false;
    bool m_x64 = false;
};
