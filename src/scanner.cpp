#include "scanner.h"
#include "value_utils.h"
#include "parse_utils.h"
#include "scan_chunk.h"
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>

// Read large regions in bounded chunks instead of one giant allocation.
// Games routinely commit heaps > 256 MB; scanning them in 64 MB chunks keeps
// peak memory bounded while still covering every byte.
static const size_t kChunk = 64 * 1024 * 1024;

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
            // Reject tokens that are not fully consumed (trailing junk like
            // "7Fzz") or that overflow a single byte (e.g. "1234" or "1FF").
            // Previously only a completely non-hex token was rejected, which
            // let "1234 56" silently truncate to byte 0x34.
            if (end != token.c_str() + token.size() || val > 0xFF) {
                result.valid = false;
                result.bytes.clear();
                result.mask.clear();
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

// Read the raw integer value per m_valueType without routing through double.
// Qword (and large Dword) values above 2^53 lose precision when converted to
// double, so bigger/smaller/between/increased/decreased comparisons must use
// these full-width integer reads instead.
uint64_t Scanner::ToUInt64(const uint8_t* data) const {
    switch (m_valueType) {
    case ValueType::Byte:  return (uint64_t)*reinterpret_cast<const uint8_t*>(data);
    case ValueType::Word:  return (uint64_t)*reinterpret_cast<const uint16_t*>(data);
    case ValueType::Dword: return (uint64_t)*reinterpret_cast<const uint32_t*>(data);
    case ValueType::Qword: return *reinterpret_cast<const uint64_t*>(data);
    default: return 0;
    }
}

bool Scanner::IsIntegerValueType() const {
    switch (m_valueType) {
    case ValueType::Byte:
    case ValueType::Word:
    case ValueType::Dword:
    case ValueType::Qword:
        return true;
    default:
        return false;
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
    // Delegate to the pure, process-independent helper so the parsing edge
    // cases (overflow truncation, hex/decimal, float) are unit-testable.
    return ParseValueToBytes(m_valueType, str, out, size, hex);
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

    // Filter to scan range if one is set (m_scanSize > 0)
    if (m_scanSize > 0) {
        std::vector<MemoryRegion> filtered;
        uintptr_t scanEnd = m_scanBase + m_scanSize;
        for (auto& r : regions) {
            uintptr_t rEnd = r.base + r.size;
            if (r.base < scanEnd && rEnd > m_scanBase) {
                filtered.push_back(r);
            }
        }
        regions = std::move(filtered);
    }

    size_t totalRegions = regions.size();

    if (unknownInit) {
        // Unknown initial value: store snapshot of all regions
        std::vector<RegionSnapshot> localSnap;
        for (size_t ri = 0; ri < totalRegions; ri++) {
            auto& r = regions[ri];
            if (r.size == 0 || vsz == 0) continue;

            // Split the region into chunks of <= kChunk bytes. Each chunk is
            // stored as its own snapshot entry; overlapping by (vsz - 1) bytes
            // keeps values that straddle a chunk edge fully readable, and the
            // per-chunk emitLen prevents NextScanWorker from emitting the same
            // absolute address twice. Small regions yield a single chunk.
            auto chunkList = ComputeScanChunks(r.size, kChunk, vsz);
            for (auto& ck : chunkList) {
                if (!m_scanning.load(std::memory_order_relaxed)) break;
                RegionSnapshot snap;
                snap.base = r.base + ck.offset;
                snap.data.resize(ck.readLen);
                // Tolerate a partial read (e.g. a committed chunk whose tail
                // borders a guard page). Keep only the readable prefix so its
                // addresses are recovered instead of dropping the whole chunk.
                size_t got = m_pm->ReadPartial(snap.base, snap.data.data(), ck.readLen);
                if (got >= vsz) {
                    snap.data.resize(got);
                    // Clamp emitLen to the readable prefix so the snapshot
                    // count math and NextScanWorker stay consistent. A fully
                    // readable chunk (got == ck.readLen) yields ck.emitLen
                    // unchanged, since ComputeScanChunks already ensures
                    // ck.emitLen <= readable.
                    size_t readable = got - vsz + 1;
                    snap.emitLen = std::min(ck.emitLen, readable);
                    localSnap.push_back(std::move(snap));
                }
            }
            m_progress = (float)(ri + 1) / totalRegions * 0.5f;
            if (!m_scanning.load(std::memory_order_relaxed)) break;
        }

        // Compute cached result count for snapshot mode. Use the per-chunk
        // emit span so overlapping chunks are not double-counted.
        size_t snapCount = 0;
        for (auto& snap : localSnap) {
            if (snap.emitLen > 0)
                snapCount += snap.emitLen;
            else if (snap.data.size() >= vsz)
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
        uint64_t targetU = 0, target2U = 0;
        bool hasTarget = false;

        if (type == ValueType::String) {
            hasTarget = !valueStr.empty();
        } else if (type == ValueType::AOB) {
            hasTarget = m_aobPattern.valid;
        } else {
            hasTarget = ParseAndWrite(valueStr, targetBuf, vsz, hex);
            targetVal = ToDouble(targetBuf);
            targetU = ToUInt64(targetBuf);
            if (scanType == 3) { // between
                ParseAndWrite(valueStr2, target2Buf, vsz, hex);
                target2Val = ToDouble(target2Buf);
                target2U = ToUInt64(target2Buf);
            }
        }

        if (!hasTarget) {
            m_scanning = false;
            return;
        }

        size_t patternLen = (type == ValueType::AOB) ? m_aobPattern.bytes.size() : vsz;
        size_t strLen = (type == ValueType::String) ? m_searchString.size() : 0;

        // patternLen / strLen / vsz are all the comparison element length, so
        // a single element width drives the chunk overlap.
        size_t elemLen = patternLen; // == vsz for numeric, == strLen for String

        for (size_t ri = 0; ri < totalRegions; ri++) {
            auto& r = regions[ri];
            if (r.size == 0 || elemLen == 0) continue;

            // Read the region in bounded chunks so committed heaps larger than
            // a few hundred MB are still fully scanned. Consecutive chunks
            // overlap by (elemLen - 1) bytes so a value straddling a chunk edge
            // is fully readable in one read; emitLen tiles the region without
            // overlap so each absolute address is emitted exactly once.
            auto chunkList = ComputeScanChunks(r.size, kChunk, elemLen);

            std::vector<uint8_t> data;
            for (auto& ck : chunkList) {
                uintptr_t chunkBase = r.base + ck.offset;
                data.resize(ck.readLen);
                // Tolerate a partial read: recover the readable prefix instead
                // of skipping the whole chunk. Do NOT zero-fill the unread tail
                // — zeros would create spurious matches for a target value of 0.
                size_t got = m_pm->ReadPartial(chunkBase, data.data(), ck.readLen);
                if (got < elemLen) {
                    continue; // nothing fully readable; keep scanning others
                }

                // Only emit within the non-overlapping head of this chunk, and
                // only over offsets that were actually read. A fully-readable
                // chunk (got == ck.readLen) leaves this identical to before.
                size_t readable = got - elemLen + 1;
                size_t scanLen = std::min(ck.emitLen, readable);

                for (size_t off = 0; off < scanLen; off++) {
                    if (off % 4096 == 0 && !m_scanning.load(std::memory_order_relaxed)) goto scan_done;
                    uintptr_t addr = chunkBase + off;
                    // Skip offsets outside the restricted scan range
                    if (m_scanSize > 0) {
                        if (addr < m_scanBase || addr + vsz > m_scanBase + m_scanSize) continue;
                    }
                    bool match = false;
                    const uint8_t* ptr = data.data() + off;

                    // String/AOB are non-numeric; Bigger/Smaller/Between have no
                    // defined ordering, so any first-scan operator falls back to
                    // Exact (memcmp-only) semantics and valueStr2 is ignored. The
                    // numeric branches below (scanType == 0 / 1 / 2 / 3) are only
                    // reached for numeric value types.
                    if (type == ValueType::String || type == ValueType::AOB) {
                        if (type == ValueType::String) {
                            match = (memcmp(ptr, m_searchString.data(), strLen) == 0);
                        } else {
                            match = true;
                            for (size_t i = 0; i < patternLen; i++) {
                                if (m_aobPattern.mask[i] && ptr[i] != m_aobPattern.bytes[i]) {
                                    match = false;
                                    break;
                                }
                            }
                        }
                    } else if (scanType == 0) {
                        match = (memcmp(ptr, targetBuf, vsz) == 0);            // exact
                    } else if (IsIntegerValueType()) {
                        // Full-width unsigned integer comparison — no double
                        // precision loss for Qword / large Dword values.
                        uint64_t curVal = ToUInt64(ptr);
                        switch (scanType) {
                        case 1: match = (curVal > targetU); break;             // bigger
                        case 2: match = (curVal < targetU); break;             // smaller
                        case 3: {                                              // between
                            uint64_t lo = std::min(targetU, target2U);
                            uint64_t hi = std::max(targetU, target2U);
                            match = (curVal >= lo && curVal <= hi);
                            break;
                        }
                        }
                    } else {
                        double curVal = ToDouble(ptr);
                        switch (scanType) {
                        case 1: match = (curVal > targetVal); break;           // bigger
                        case 2: match = (curVal < targetVal); break;           // smaller
                        case 3: {                                              // between
                            double lo = std::min(targetVal, target2Val);
                            double hi = std::max(targetVal, target2Val);
                            match = (curVal >= lo && curVal <= hi);
                            break;
                        }
                        }
                    }

                    if (match) {
                        localResults.push_back(addr);
                        if (localResults.size() >= 5000000) goto scan_done;
                    }
                }

                m_cachedResultCount.store(localResults.size(), std::memory_order_relaxed);
                if (!m_scanning.load(std::memory_order_relaxed)) goto scan_done;
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
        uint64_t targetU = 0, target2U = 0;

        if (needTarget && IsNumericType(m_valueType)) {
            ParseAndWrite(valueStr, targetBuf, vsz, hex);
            targetVal = ToDouble(targetBuf);
            targetU = ToUInt64(targetBuf);
            if (nextScanType == 3) {
                ParseAndWrite(valueStr2, target2Buf, vsz, hex);
                target2Val = ToDouble(target2Buf);
                target2U = ToUInt64(target2Buf);
            }
        }

        size_t totalSnaps = m_snapshot.size();
        for (size_t si = 0; si < totalSnaps; si++) {
            auto& snap = m_snapshot[si];
            std::vector<uint8_t> current(snap.data.size());
            // Tolerate a partial read of the live bytes: derive the scan span
            // from what was actually read rather than the buffer size, so a
            // chunk that now borders a guard page still contributes its
            // readable prefix instead of being skipped entirely.
            size_t got = m_pm->ReadPartial(snap.base, current.data(), current.size());
            if (got < vsz) {
                m_progress = (float)(si + 1) / totalSnaps;
                continue;
            }

            // Chunked snapshots overlap the next chunk by (vsz - 1) bytes so a
            // value straddling the boundary stays readable; only emit the
            // non-overlapping head (emitLen offsets) so each absolute address
            // is produced exactly once. emitLen == 0 => legacy non-chunked
            // snapshot: emit every fully-readable offset. A full read
            // (got == current.size()) leaves readable identical to before.
            size_t readable = got - vsz + 1;
            size_t scanLen = (snap.emitLen > 0) ? std::min(snap.emitLen, readable)
                                                : readable;
            for (size_t off = 0; off < scanLen; off++) {
                if (off % 4096 == 0 && !m_scanning.load(std::memory_order_relaxed)) goto snap_done;
                // Skip offsets outside the restricted scan range
                if (m_scanSize > 0) {
                    uintptr_t addr = snap.base + off;
                    if (addr < m_scanBase || addr + vsz > m_scanBase + m_scanSize) continue;
                }
                const uint8_t* cur = current.data() + off;
                const uint8_t* prev = snap.data.data() + off;
                bool match = false;

                bool isInt = IsIntegerValueType();
                double curVal = (IsNumericType(m_valueType) && !isInt) ? ToDouble(cur) : 0;
                double prevVal = (IsNumericType(m_valueType) && !isInt) ? ToDouble(prev) : 0;
                // Full-width integer values for Byte/Word/Dword/Qword to avoid
                // double precision loss above 2^53.
                uint64_t curU = isInt ? ToUInt64(cur) : 0;
                uint64_t prevU = isInt ? ToUInt64(prev) : 0;

                // Bigger(1)/Smaller(2)/Between(3) are undefined for non-numeric
                // String/AOB value types: there is no ordinal value to order
                // against and the target value is never parsed for them, so
                // running the numeric comparison would emit garbage (every
                // address for Between, none for Bigger/Smaller). Skip those
                // comparisons so no result is emitted — the operator is
                // meaningless, not an error. Exact(0), Changed(4), Unchanged(5),
                // Increased(6) and Decreased(7) remain valid for every type.
                if (IsNumericType(m_valueType) || nextScanType == 0 ||
                    nextScanType >= 4) {
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
                        // m_searchString is fixed from the first scan so it is
                        // already bounded by vsz; clamp defensively for symmetry
                        // with the explicit-results path.
                        size_t cmpLen = std::min(m_searchString.size(), vsz);
                        match = (m_searchString.size() <= vsz &&
                                 memcmp(cur, m_searchString.data(), cmpLen) == 0);
                    } else {
                        match = (memcmp(cur, targetBuf, vsz) == 0);
                    }
                    break;
                }
                case 1: match = isInt ? (curU > targetU) : (curVal > targetVal); break;  // bigger
                case 2: match = isInt ? (curU < targetU) : (curVal < targetVal); break;  // smaller
                case 3: {                                            // between
                    if (isInt) {
                        uint64_t lo = std::min(targetU, target2U);
                        uint64_t hi = std::max(targetU, target2U);
                        match = (curU >= lo && curU <= hi);
                    } else {
                        double lo = std::min(targetVal, target2Val);
                        double hi = std::max(targetVal, target2Val);
                        match = (curVal >= lo && curVal <= hi);
                    }
                    break;
                }
                case 4: match = (memcmp(cur, prev, vsz) != 0); break;      // changed
                case 5: match = (memcmp(cur, prev, vsz) == 0); break;      // unchanged
                case 6: match = isInt ? (curU > prevU) : (curVal > prevVal); break;      // increased
                case 7: match = isInt ? (curU < prevU) : (curVal < prevVal); break;      // decreased
                }
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
    uint64_t targetU = 0, target2U = 0;

    if (needTarget && IsNumericType(m_valueType)) {
        ParseAndWrite(valueStr, targetBuf, vsz, hex);
        targetVal = ToDouble(targetBuf);
        targetU = ToUInt64(targetBuf);
        if (nextScanType == 3) {
            ParseAndWrite(valueStr2, target2Buf, vsz, hex);
            target2Val = ToDouble(target2Buf);
            target2U = ToUInt64(target2Buf);
        }
    }

    size_t total = currentResults.size();
    size_t batchSize = 4096;
    std::vector<uint8_t> readBuf(batchSize * vsz);

    for (size_t base = 0; base < total; base += batchSize) {
        size_t count = std::min(batchSize, total - base);

        // Read each address individually; zero-fill any slot whose read
        // fails so downstream comparisons are deterministic instead of
        // matching against stale bytes left over from a prior iteration.
        for (size_t i = 0; i < count; i++) {
            uintptr_t addr = currentResults[base + i];
            if (!m_pm->Read(addr, readBuf.data() + i * vsz, vsz)) {
                memset(readBuf.data() + i * vsz, 0, vsz);
            }
        }

        for (size_t i = 0; i < count; i++) {
            uintptr_t addr = currentResults[base + i];
            const uint8_t* cur = readBuf.data() + i * vsz;
            const uint8_t* prev = m_prevValues.data() + (base + i) * vsz;
            bool match = false;

            if (m_valueType == ValueType::String) {
                if (nextScanType == 0) {
                    // Each read slot is only vsz bytes wide (vsz was fixed by
                    // the first scan's search-string length). A next-scan input
                    // longer than vsz can never match a vsz-byte slot, so reject
                    // it instead of reading past the end of readBuf.
                    size_t cmpLen = std::min(valueStr.size(), vsz);
                    match = (valueStr.size() <= vsz &&
                             memcmp(cur, valueStr.data(), cmpLen) == 0);
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
            } else if (IsIntegerValueType()) {
                // Full-width integer comparison — no double precision loss
                // for Qword / large Dword values above 2^53.
                uint64_t curU = ToUInt64(cur);
                uint64_t prevU = ToUInt64(prev);

                switch (nextScanType) {
                case 0: match = (memcmp(cur, targetBuf, vsz) == 0); break;
                case 1: match = (curU > targetU); break;
                case 2: match = (curU < targetU); break;
                case 3: {
                    uint64_t lo = std::min(targetU, target2U);
                    uint64_t hi = std::max(targetU, target2U);
                    match = (curU >= lo && curU <= hi);
                    break;
                }
                case 4: match = (memcmp(cur, prev, vsz) != 0); break;
                case 5: match = (memcmp(cur, prev, vsz) == 0); break;
                case 6: match = (curU > prevU); break;
                case 7: match = (curU < prevU); break;
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
    size_t aobLen = (m_valueType == ValueType::AOB) ? m_aobPattern.bytes.size() : 8;
    return ::ReadValueString(*m_pm, addr, m_valueType, aobLen);
}
