#pragma once
#include "types.h"
#include "process_manager.h"
#include <atomic>
#include <mutex>
#include <thread>

class Scanner {
public:
    Scanner() = default;
    ~Scanner();

    void SetProcess(ProcessManager* pm) { m_pm = pm; }

    // Restrict scanning to a memory range (e.g. a single module).
    // base=0, size=0 means scan all regions.
    void SetScanRange(uintptr_t base, size_t size) { m_scanBase = base; m_scanSize = size; }

    // scanType: 0=exact, 1=bigger, 2=smaller, 3=between, 4=unknown
    void NewScanAsync(ValueType type, int scanType,
                      const std::string& valueStr, const std::string& valueStr2,
                      bool hex, bool writableOnly);

    // nextScanType: 0=exact, 1=bigger, 2=smaller, 3=between,
    //               4=changed, 5=unchanged, 6=increased, 7=decreased
    bool NextScanAsync(int nextScanType,
                       const std::string& valueStr, const std::string& valueStr2,
                       bool hex);

    void Reset();

    bool IsScanning() const { return m_scanning; }
    void RequestCancel() { m_scanning.store(false, std::memory_order_relaxed); }
    float GetProgress() const { return m_progress; }
    size_t GetResultCount() const;
    bool IsFirstScan() const { return m_firstScan.load(std::memory_order_acquire); }
    ValueType GetValueType() const { return m_valueType; }

    std::vector<uintptr_t> GetResultsCopy() const;
    std::string ReadValueString(uintptr_t addr) const;

    struct BytePattern {
        std::vector<uint8_t> bytes;
        std::vector<bool> mask;
        bool valid = false;
    };
    static BytePattern ParseAOB(const std::string& pattern);

    const BytePattern& GetAOBPattern() const { return m_aobPattern; }

private:
    ProcessManager* m_pm = nullptr;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_scanning{false};
    std::atomic<float> m_progress{0.0f};
    std::thread m_thread;

    std::vector<uintptr_t> m_results;
    std::atomic<bool> m_firstScan{true};
    ValueType m_valueType = ValueType::Dword;

    // For changed/unchanged: snapshot of previous values
    struct RegionSnapshot {
        uintptr_t base = 0;
        std::vector<uint8_t> data;
    };
    std::vector<RegionSnapshot> m_snapshot;
    std::atomic<bool> m_hasSnapshot{false};
    std::atomic<size_t> m_cachedResultCount{0};

    // Previous values for explicit results (changed/unchanged/increased/decreased)
    std::vector<uint8_t> m_prevValues;
    size_t m_valueSize = 0;

    BytePattern m_aobPattern;
    std::string m_searchString;

    // Scan range restriction (0/0 = scan everything)
    uintptr_t m_scanBase = 0;
    size_t m_scanSize = 0;

    void NewScanWorker(ValueType type, int scanType,
                       std::string valueStr, std::string valueStr2,
                       bool hex, bool writableOnly);

    bool NextScanWorker(int nextScanType,
                        std::string valueStr, std::string valueStr2,
                        bool hex);

    double ToDouble(const uint8_t* data) const;
    // Integer-aware read that avoids the precision loss of routing large
    // 8-byte values through double (uint64_t -> double loses bits above 2^53).
    uint64_t ToUInt64(const uint8_t* data) const;
    // True for Byte/Word/Dword/Qword — the types that must use integer
    // comparisons rather than the double path (which is for Float32/Float64).
    bool IsIntegerValueType() const;
    std::string FormatValue(const uint8_t* data) const;
    bool ParseAndWrite(const std::string& str, uint8_t* out, size_t size, bool hex) const;
    void StorePrevValues();
};
