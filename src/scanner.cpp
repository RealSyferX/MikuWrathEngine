#include "scanner.h"
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>

Scanner::~Scanner() {
    m_scanning.store(false, std::memory_order_relaxed);
    if (m_thread.joinable()) m_thread.join();
}

// ============================================================
// AOB Pattern Parsing — supports "7F ?? 90" style wildcards
// ============================================================
Scanner::BytePattern Scanner::ParseAOB(const std::string& pattern) {
    BytePattern result;
    std::istringstream iss(pattern);
    std::string token;
    while (iss >> token) {
        if (token == "??" || token == "?") {
            result.bytes.push_back(0);
            result.mask.push_back(false);
        } else {
            char* end = nullptr;
            unsigned long val = strtoul(token.c_str(), &end, 16);
            if (end == token.c_str()) {
                result.valid = false;
                return result;
            }
            result.bytes.push_back((uint8_t)val);
            result.mask.push_back(true);
        }
    }
    result.valid = !result.bytes.empty();
    return result;
}

// ============================================================
// Value conversion helpers
// ============================================================
double Scanner::ToDouble(const uint8_t* data) const {
    switch (m_valueType) {
    case ValueType::Byte:    return (double)*reinterpret_cast<const uint8_t*>(data);
    case ValueType::Word:    return (double)*reinterpret_cast<const uint16_t*>(data);
    case ValueType::Dword:   return (double)*reinterpret_cast<const uint32_t*>(data);
    case ValueType::Qword:   return (double)*reinterpret_cast<const uint64_t*>(data);
    case ValueType::Float32: return (double)*reinterpret_cast<const float*>(data);
    case ValueType::Float64: return *reinterpret_cast<const double*>(data);
    default: return 0;
    }
}

std::string Scanner::FormatValue(const uint8_t* data) const {
    char buf[64];
    switch (m_valueType) {
    case ValueType::Byte:    snprintf(buf, sizeof(buf), "%u", *data); break;
    case ValueType::Word:    snprintf(buf, sizeof(buf), "%u", *reinterpret_cast<const uint16_t*>(data)); break;
    case ValueType::Dword:   snprintf(buf, sizeof(buf), "%u", *reinterpret_cast<const uint32_t*>(data)); break;
    case ValueType::Qword:   snprintf(buf, sizeof(buf), "%llu", *reinterpret_cast<const uint64_t*>(data)); break;
    case ValueType::Float32: snprintf(buf, sizeof(buf), "%g", *reinterpret_cast<const float*>(data)); break;
    case ValueType::Float64: snprintf(buf, sizeof(buf), "%g", *reinterpret_cast<const double*>(data)); break;
    default: buf[0] = '\0'; break;
    }
    return buf;
}

bool Scanner::ParseAndWrite(const std::string& str, uint8_t* out, size_t size, bool hex) const {
    int base = hex ? 16 : 10;
    try {
        switch (m_valueType) {
        case ValueType::Byte: {
            auto v = (uint8_t)std::stoul(str, nullptr, base);
            memcpy(out, &v, 1); return true;
        }
        case ValueType::Word: {
            auto v = (uint16_t)std::stoul(str, nullptr, base);
            memcpy(out, &v, 2); return true;
        }
        case ValueType::Dword: {
            auto v = (uint32_t)std::stoul(str, nullptr, base);
            memcpy(out, &v, 4); return true;
        }
        case ValueType::Qword: {
            auto v = (uint64_t)std::stoull(str, nullptr, base);
            memcpy(out, &v, 8); return true;
        }
        case ValueType::Float32: {
            float v = std::stof(str);
            memcpy(out, &v, 4); return true;
        }
        case ValueType::Float64: {
            double v = std::stod(str);
            memcpy(out, &v, 8); return true;
        }
        default: return false;
        }
    } catch (...) { return false; }
}

// ============================================================
// Async scan entry points
// ============================================================
void Scanner::NewScanAsync(ValueType type, int scanType,
                           const std::string& valueStr, const std::string& valueStr2,
                           bool hex, bool writableOnly) {
    if (m_scanning) return;
    if (m_thread.joinable()) m_thread.join();

    m_valueType = type;
    m_scanning = true;
    m_progress = 0.0f;

    m_thread = std::thread(&Scanner::NewScanWorker, this,
                           type, scanType, valueStr, valueStr2, hex, writableOnly);
}

bool Scanner::NextScanAsync(int nextScanType,
                            const std::string& valueStr, const std::string& valueStr2,
                            bool hex) {
    if (m_scanning) return false;
    if (m_firstScan.load(std::memory_order_acquire)) return false;
    if (m_thread.joinable()) m_thread.join();

    m_scanning = true;
    m_progress = 0.0f;

    m_thread = std::thread(&Scanner::NextScanWorker, this,
                           nextScanType, valueStr, valueStr2, hex);
    return true;
}

void Scanner::Reset() {
    if (m_scanning) {
        m_scanning = false;
        if (m_thread.joinable()) m_thread.join();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_results.clear();
    m_snapshot.clear();
    m_prevValues.clear();
    m_hasSnapshot.store(false, std::memory_order_release);
    m_firstScan.store(true, std::memory_order_release);
    m_cachedResultCount.store(0, std::memory_order_relaxed);
    m_progress = 0.0f;
}

size_t Scanner::GetResultCount() const {
    if (m_hasSnapshot.load(std::memory_order_acquire)) {
        return m_cachedResultCount.load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_results.size();
}

std::vector<uintptr_t> Scanner::GetResultsCopy() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_results;
}

// ============================================================
// New Scan Worker
// ============================================================
void Scanner::NewScanWorker(ValueType type, int scanType,
                            std::string valueStr, std::string valueStr2,
                            bool hex, bool writableOnly) {
    bool unknownInit = (scanType == 4);

    if (type == ValueType::String) {
        m_searchString = valueStr;
    }
    if (type == ValueType::AOB) {
        m_aobPattern = ParseAOB(valueStr);
        if (!m_aobPattern.valid) {
            m_scanning = false;
            return;
        }
    }

    // Compute the per-element byte width. For numeric types this is the
    // fixed type size; for AOB/String it is the pattern/string length so
    // that snapshot mode (which relies on m_valueSize) works correctly.
    size_t vsz;
    if (type == ValueType::AOB) {
        vsz = m_aobPattern.bytes.size();
    } else if (type == ValueType::String) {
        vsz = m_searchString.size();
    } else {
        vsz = ValueTypeSize(type);
    }
    m_valueSize = vsz;

    auto regions = m_pm->EnumerateRegions(writableOnly);
    size_t totalRegions = regions.size();

    if (unknownInit) {
        // Unknown initial value: store snapshot of all regions
        std::vector<RegionSnapshot> localSnap;
        for (size_t ri = 0; ri < totalRegions; ri++) {
            auto& r = regions[ri];
            if (r.size == 0 || r.size > 256 * 1024 * 1024) continue; // skip huge regions
            RegionSnapshot snap;
            snap.base = r.base;
            snap.data.resize(r.size);
            if (m_pm->Read(r.base, snap.data.data(), r.size)) {
                localSnap.push_back(std::move(snap));
            }
            m_progress = (float)(ri + 1) / totalRegions * 0.5f;
        }

        // Compute cached result count for snapshot mode
        size_t snapCount = 0;
        for (auto& snap : localSnap) {
            if (snap.data.size() >= vsz)
                snapCount += snap.data.size() - vsz + 1;
        }
        m_cachedResultCount.store(snapCount, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot = std::move(localSnap);
            m_results.clear();
            m_hasSnapshot.store(true, std::memory_order_release);
            m_prevValues.clear();
        }
        m_firstScan.store(false, std::memory_order_release);
    } else {
        // Exact / bigger / smaller / between
        std::vector<uintptr_t> localResults;

        // Parse target value(s)
        uint8_t targetBuf[8] = {};
        uint8_t target2Buf[8] = {};
        double targetVal = 0, target2Val = 0;
        bool hasTarget = false;

        if (type == ValueType::String) {
            hasTarget = !valueStr.empty();
        } else if (type == ValueType::AOB) {
            hasTarget = m_aobPattern.valid;
        } else {
            hasTarget = ParseAndWrite(valueStr, targetBuf, vsz, hex);
            targetVal = ToDouble(targetBuf);
            if (scanType == 3) { // between
                ParseAndWrite(valueStr2, target2Buf, vsz, hex);
                target2Val = ToDouble(target2Buf);
            }
        }

        if (!hasTarget) {
            m_scanning = false;
            return;
        }

        size_t patternLen = (type == ValueType::AOB) ? m_aobPattern.bytes.size() : vsz;
        size_t strLen = (type == ValueType::String) ? m_searchString.size() : 0;

        for (size_t ri = 0; ri < totalRegions; ri++) {
            auto& r = regions[ri];
            if (r.size == 0 || r.size > 256 * 1024 * 1024) continue;

            std::vector<uint8_t> data(r.size);
            if (!m_pm->Read(r.base, data.data(), r.size)) {
                m_progress = (float)(ri + 1) / totalRegions;
                continue;
            }

            size_t scanLen;
            if (type == ValueType::String)
                scanLen = (r.size >= strLen) ? r.size - strLen + 1 : 0;
            else if (type == ValueType::AOB)
                scanLen = (r.size >= patternLen) ? r.size - patternLen + 1 : 0;
            else
                scanLen = (r.size >= vsz) ? r.size - vsz + 1 : 0;

            for (size_t off = 0; off < scanLen; off++) {
                if (off % 4096 == 0 && !m_scanning.load(std::memory_order_relaxed)) goto scan_done;
                bool match = false;
                const uint8_t* ptr = data.data() + off;

                if (type == ValueType::String) {
                    match = (memcmp(ptr, m_searchString.data(), strLen) == 0);
                } else if (type == ValueType::AOB) {
                    match = true;
                    for (size_t i = 0; i < patternLen; i++) {
                        if (m_aobPattern.mask[i] && ptr[i] != m_aobPattern.bytes[i]) {
                            match = false;
                            break;
                        }
                    }
                } else {
                    double curVal = ToDouble(ptr);
                    switch (scanType) {
                    case 0: match = (memcmp(ptr, targetBuf, vsz) == 0); break; // exact
                    case 1: match = (curVal > targetVal); break;               // bigger
                    case 2: match = (curVal < targetVal); break;               // smaller
                    case 3: {                                            // between
                        double lo = std::min(targetVal, target2Val);
                        double hi = std::max(targetVal, target2Val);
                        match = (curVal >= lo && curVal <= hi);
                        break;
                    }
                    }
                }

                if (match) {
                    localResults.push_back(r.base + off);
                    if (localResults.size() >= 5000000) goto scan_done;
                }
            }

            m_progress = (float)(ri + 1) / totalRegions;
            m_cachedResultCount.store(localResults.size(), std::memory_order_relaxed);
            if (!m_scanning.load(std::memory_order_relaxed)) goto scan_done;
        }

    scan_done:
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_results = std::move(localResults);
            m_snapshot.clear();
            m_hasSnapshot.store(false, std::memory_order_release);
        }

        StorePrevValues();
        m_firstScan.store(false, std::memory_order_release);
    }

    m_progress = 1.0f;
    m_scanning = false;
}

// ============================================================
// Next Scan Worker
// ============================================================
bool Scanner::NextScanWorker(int nextScanType,
                             std::string valueStr, std::string valueStr2,
                             bool hex) {
    size_t vsz = m_valueSize;

    // Handle snapshot mode (from Unknown Initial Value)
    if (m_hasSnapshot.load(std::memory_order_acquire)) {
        // Guard: a zero value size (e.g. empty AOB/String) makes the
        // scanLen math below read out of bounds. Bail early instead.
        if (m_valueSize == 0) { m_scanning = false; return false; }

        std::vector<uintptr_t> localResults;

        bool needTarget = (nextScanType <= 3); // exact/bigger/smaller/between need a target
        uint8_t targetBuf[8] = {}, target2Buf[8] = {};
        double targetVal = 0, target2Val = 0;

        if (needTarget && IsNumericType(m_valueType)) {
            ParseAndWrite(valueStr, targetBuf, vsz, hex);
            targetVal = ToDouble(targetBuf);
            if (nextScanType == 3) {
                ParseAndWrite(valueStr2, target2Buf, vsz, hex);
                target2Val = ToDouble(target2Buf);
            }
        }

        size_t totalSnaps = m_snapshot.size();
        for (size_t si = 0; si < totalSnaps; si++) {
            auto& snap = m_snapshot[si];
            std::vector<uint8_t> current(snap.data.size());
            if (!m_pm->Read(snap.base, current.data(), current.size())) {
                m_progress = (float)(si + 1) / totalSnaps;
                continue;
            }

            size_t scanLen = (current.size() >= vsz) ? current.size() - vsz + 1 : 0;
            for (size_t off = 0; off < scanLen; off++) {
                if (off % 4096 == 0 && !m_scanning.load(std::memory_order_relaxed)) goto snap_done;
                const uint8_t* cur = current.data() + off;
                const uint8_t* prev = snap.data.data() + off;
                bool match = false;

                double curVal = IsNumericType(m_valueType) ? ToDouble(cur) : 0;
                double prevVal = IsNumericType(m_valueType) ? ToDouble(prev) : 0;

                switch (nextScanType) {
                case 0: { // exact
                    // AOB/String have no targetBuf (IsNumericType is false,
                    // so ParseAndWrite was skipped). Match against the
                    // parsed pattern / search string instead.
                    if (m_valueType == ValueType::AOB) {
                        match = true;
                        for (size_t j = 0; j < m_aobPattern.bytes.size(); j++) {
                            if (m_aobPattern.mask[j] && cur[j] != m_aobPattern.bytes[j]) {
                                match = false; break;
                            }
                        }
                    } else if (m_valueType == ValueType::String) {
                        match = (memcmp(cur, m_searchString.data(), m_searchString.size()) == 0);
                    } else {
                        match = (memcmp(cur, targetBuf, vsz) == 0);
                    }
                    break;
                }
                case 1: match = (curVal > targetVal); break;               // bigger
                case 2: match = (curVal < targetVal); break;               // smaller
                case 3: {                                            // between
                    double lo = std::min(targetVal, target2Val);
                    double hi = std::max(targetVal, target2Val);
                    match = (curVal >= lo && curVal <= hi);
                    break;
                }
                case 4: match = (memcmp(cur, prev, vsz) != 0); break;      // changed
                case 5: match = (memcmp(cur, prev, vsz) == 0); break;      // unchanged
                case 6: match = (curVal > prevVal); break;                 // increased
                case 7: match = (curVal < prevVal); break;                 // decreased
                }

                if (match) {
                    localResults.push_back(snap.base + off);
                    if (localResults.size() >= 5000000) goto snap_done;
                }
            }

            m_progress = (float)(si + 1) / totalSnaps;
            m_cachedResultCount.store(localResults.size(), std::memory_order_relaxed);
            if (!m_scanning.load(std::memory_order_relaxed)) goto snap_done;
        }

    snap_done:
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_results = std::move(localResults);
            m_snapshot.clear();
            m_hasSnapshot.store(false, std::memory_order_release);
        }
        StorePrevValues();
        m_firstScan.store(false, std::memory_order_release);
        m_progress = 1.0f;
        m_scanning = false;
        return true;
    }

    // Explicit results mode
    std::vector<uintptr_t> currentResults;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        currentResults = m_results;
    }

    if (currentResults.empty()) {
        m_progress = 1.0f;
        m_scanning = false;
        return false;
    }

    std::vector<uintptr_t> filtered;

    bool needTarget = (nextScanType <= 3);
    uint8_t targetBuf[8] = {}, target2Buf[8] = {};
    double targetVal = 0, target2Val = 0;

    if (needTarget && IsNumericType(m_valueType)) {
        ParseAndWrite(valueStr, targetBuf, vsz, hex);
        targetVal = ToDouble(targetBuf);
        if (nextScanType == 3) {
            ParseAndWrite(valueStr2, target2Buf, vsz, hex);
            target2Val = ToDouble(target2Buf);
        }
    }

    size_t total = currentResults.size();
    size_t batchSize = 4096;
    std::vector<uint8_t> readBuf(batchSize * vsz);

    for (size_t base = 0; base < total; base += batchSize) {
        size_t count = std::min(batchSize, total - base);

        // Batch read: try to read all at once
        uintptr_t startAddr = currentResults[base];
        bool allRead = true;

        for (size_t i = 0; i < count; i++) {
            uintptr_t addr = currentResults[base + i];
            if (!m_pm->Read(addr, readBuf.data() + i * vsz, vsz)) {
                allRead = false;
                break;
            }
        }

        if (!allRead) {
            // Fallback: read individually
            for (size_t i = 0; i < count; i++) {
                uintptr_t addr = currentResults[base + i];
                if (!m_pm->Read(addr, readBuf.data() + i * vsz, vsz)) {
                    continue;
                }
            }
        }

        for (size_t i = 0; i < count; i++) {
            uintptr_t addr = currentResults[base + i];
            const uint8_t* cur = readBuf.data() + i * vsz;
            const uint8_t* prev = m_prevValues.data() + (base + i) * vsz;
            bool match = false;

            if (m_valueType == ValueType::String) {
                if (nextScanType == 0) {
                    match = (memcmp(cur, valueStr.data(), valueStr.size()) == 0);
                } else {
                    match = false;
                }
            } else if (m_valueType == ValueType::AOB) {
                if (nextScanType == 0) {
                    match = true;
                    for (size_t j = 0; j < m_aobPattern.bytes.size(); j++) {
                        if (m_aobPattern.mask[j] && cur[j] != m_aobPattern.bytes[j]) {
                            match = false;
                            break;
                        }
                    }
                } else {
                    match = false;
                }
            } else {
                double curVal = ToDouble(cur);
                double prevVal = ToDouble(prev);

                switch (nextScanType) {
                case 0: match = (memcmp(cur, targetBuf, vsz) == 0); break;
                case 1: match = (curVal > targetVal); break;
                case 2: match = (curVal < targetVal); break;
                case 3: {
                    double lo = std::min(targetVal, target2Val);
                    double hi = std::max(targetVal, target2Val);
                    match = (curVal >= lo && curVal <= hi);
                    break;
                }
                case 4: match = (memcmp(cur, prev, vsz) != 0); break;
                case 5: match = (memcmp(cur, prev, vsz) == 0); break;
                case 6: match = (curVal > prevVal); break;
                case 7: match = (curVal < prevVal); break;
                }
            }

            if (match) {
                filtered.push_back(addr);
                if (filtered.size() >= 5000000) goto filter_done;
            }
        }

        m_progress = (float)(base + count) / total;
        m_cachedResultCount.store(filtered.size(), std::memory_order_relaxed);
        if (!m_scanning.load(std::memory_order_relaxed)) goto filter_done;
    }

filter_done:
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_results = std::move(filtered);
    }
    StorePrevValues();
    m_firstScan.store(false, std::memory_order_release);

    m_progress = 1.0f;
    m_scanning = false;
    return true;
}

// ============================================================
// Store previous values for changed/unchanged/increased/decreased
// ============================================================
void Scanner::StorePrevValues() {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t vsz = m_valueSize;
    if (vsz == 0 || m_results.empty()) {
        m_prevValues.clear();
        return;
    }

    m_prevValues.resize(m_results.size() * vsz);
    for (size_t i = 0; i < m_results.size(); i++) {
        if (i % 4096 == 0 && !m_scanning.load(std::memory_order_relaxed)) break;
        if (!m_pm->Read(m_results[i], m_prevValues.data() + i * vsz, vsz)) {
            memset(m_prevValues.data() + i * vsz, 0, vsz);
        }
    }
}

// ============================================================
// Read current value at address as display string
// ============================================================
std::string Scanner::ReadValueString(uintptr_t addr) const {
    if (!m_pm || !m_pm->IsOpen()) return "??";

    switch (m_valueType) {
    case ValueType::Byte: {
        uint8_t v; if (m_pm->Read(addr, &v, 1)) return std::to_string(v);
        return "??";
    }
    case ValueType::Word: {
        uint16_t v; if (m_pm->Read(addr, &v, 2)) return std::to_string(v);
        return "??";
    }
    case ValueType::Dword: {
        uint32_t v; if (m_pm->Read(addr, &v, 4)) return std::to_string(v);
        return "??";
    }
    case ValueType::Qword: {
        uint64_t v; if (m_pm->Read(addr, &v, 8)) return std::to_string((unsigned long long)v);
        return "??";
    }
    case ValueType::Float32: {
        float v; if (m_pm->Read(addr, &v, 4)) { char b[32]; snprintf(b, sizeof(b), "%g", v); return b; }
        return "??";
    }
    case ValueType::Float64: {
        double v; if (m_pm->Read(addr, &v, 8)) { char b[32]; snprintf(b, sizeof(b), "%g", v); return b; }
        return "??";
    }
    case ValueType::String: {
        char buf[64] = {};
        if (m_pm->Read(addr, buf, 63)) return std::string(buf);
        return "??";
    }
    case ValueType::AOB: {
        size_t sz = m_aobPattern.bytes.size();
        if (sz == 0 || sz > 64) return "??";
        uint8_t buf[64];
        if (m_pm->Read(addr, buf, sz)) {
            std::string r;
            char h[4];
            for (size_t i = 0; i < sz; i++) {
                snprintf(h, sizeof(h), "%02X ", buf[i]);
                r += h;
            }
            return r;
        }
        return "??";
    }
    }
    return "??";
}
