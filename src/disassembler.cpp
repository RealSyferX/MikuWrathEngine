#include "disassembler.h"
#include "process_manager.h"
#include <cstring>
#include <algorithm>

Disassembler::~Disassembler() {
    Shutdown();
}

bool Disassembler::Init(bool x64) {
    Shutdown();
    cs_mode mode = x64 ? CS_MODE_64 : CS_MODE_32;
    if (cs_open(CS_ARCH_X86, mode, &m_handle) != CS_ERR_OK)
        return false;
    cs_option(m_handle, CS_OPT_DETAIL, CS_OPT_OFF);
    m_initialized = true;
    m_x64 = x64;
    return true;
}

void Disassembler::Shutdown() {
    if (m_initialized) {
        cs_close(&m_handle);
        m_handle = 0;
        m_initialized = false;
    }
}

std::vector<DisasmInstruction> Disassembler::Disassemble(
    uint64_t address, const uint8_t* data, size_t size, size_t maxCount) const {

    std::vector<DisasmInstruction> result;
    if (!m_initialized || !data || size == 0) return result;

    cs_insn* insns = nullptr;
    size_t count = cs_disasm(m_handle, data, size, address, maxCount, &insns);
    if (count == 0) return result;

    result.reserve(count);
    for (size_t i = 0; i < count; i++) {
        DisasmInstruction inst = {};
        inst.address = insns[i].address;
        inst.size = (uint8_t)insns[i].size;
        size_t copyLen = std::min((size_t)insns[i].size, (size_t)16);
        memcpy(inst.bytes, insns[i].bytes, copyLen);
        strncpy(inst.mnemonic, insns[i].mnemonic, sizeof(inst.mnemonic) - 1);
        strncpy(inst.opStr, insns[i].op_str, sizeof(inst.opStr) - 1);
        result.push_back(inst);
    }

    cs_free(insns, count);
    return result;
}

uintptr_t Disassembler::FindPreviousInstruction(uintptr_t address,
                                                  const ProcessManager* pm,
                                                  size_t lookback) const {
    if (!m_initialized || !pm || !pm->IsOpen()) return address;

    uintptr_t backAddr = (address > lookback) ? address - lookback : 0;
    size_t backSize = address - backAddr;
    if (backSize == 0) return address;

    std::vector<uint8_t> buf(backSize);
    size_t got = pm->ReadPartial(backAddr, buf.data(), backSize);
    if (got == 0) return address;

    auto insts = Disassemble(backAddr, buf.data(), got, 64);

    uintptr_t prevStart = backAddr;
    for (auto& inst : insts) {
        if (inst.address >= address) break;
        if (inst.address + inst.size == address) {
            return inst.address;
        }
        prevStart = inst.address;
    }

    // If no exact match, return the last instruction before our target
    return prevStart;
}
