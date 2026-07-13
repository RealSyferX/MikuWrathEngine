// ============================================================
// MikuWrathEngine — pure logic unit tests
//
// No external test framework. A tiny CHECK macro prints failures and
// increments a counter; main() returns non-zero if any check failed.
//
// Only genuinely pure / process-independent logic is exercised here:
//   - Scanner::ParseAOB           (public static, pure)
//   - ParseValueToBytes           (parse_utils.h — backs Scanner::ParseAndWrite)
//   - SplitModuleOffset /
//     ParsePlainHexAddress        (parse_utils.h — backs ParseAddressString)
//   - Settings::Load / Save       (settings.h — INI parse, clamping, CR/LF strip)
// ============================================================
#include "scanner.h"
#include "parse_utils.h"
#include "scan_chunk.h"
#include "types.h"
#include "settings.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                  \
    } while (0)

// --- helpers ------------------------------------------------------------

static uint64_t ReadLE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// ============================================================
// ParseAOB
// ============================================================
static void test_parse_aob() {
    // Valid pattern with wildcard: "7F ?? 90"
    auto p = Scanner::ParseAOB("7F ?? 90");
    CHECK(p.valid);
    CHECK(p.bytes.size() == 3);
    CHECK(p.mask.size() == 3);
    CHECK(p.bytes[0] == 0x7F && p.mask[0] == true);
    CHECK(p.mask[1] == false);            // wildcard: masked out
    CHECK(p.bytes[2] == 0x90 && p.mask[2] == true);

    // Single '?' also treated as wildcard
    auto q = Scanner::ParseAOB("AA ? BB");
    CHECK(q.valid);
    CHECK(q.bytes.size() == 3);
    CHECK(q.mask[1] == false);

    // Empty string -> invalid (no bytes)
    auto e = Scanner::ParseAOB("");
    CHECK(!e.valid);
    CHECK(e.bytes.empty());

    // Whitespace-only -> invalid
    auto ws = Scanner::ParseAOB("   ");
    CHECK(!ws.valid);

    // Non-hex token -> invalid
    auto z = Scanner::ParseAOB("ZZ");
    CHECK(!z.valid);

    // Mixed valid then invalid -> invalid (bails on the bad token)
    auto m = Scanner::ParseAOB("7F GG");
    CHECK(!m.valid);
}

// ============================================================
// ParseValueToBytes (backs Scanner::ParseAndWrite)
// ============================================================
static void test_parse_value_to_bytes() {
    uint8_t buf[8];

    // ---- Byte ----
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Byte, "255", buf, 1, false));
    CHECK(buf[0] == 255);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Byte, "FF", buf, 1, true));   // hex
    CHECK(buf[0] == 0xFF);
    // Overflow truncation: 256 -> low byte 0
    memset(buf, 0xAB, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Byte, "256", buf, 1, false));
    CHECK(buf[0] == 0);

    // ---- Word ----
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Word, "65535", buf, 2, false));
    CHECK(ReadLE(buf, 2) == 65535);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Word, "1234", buf, 2, true));  // hex 0x1234
    CHECK(ReadLE(buf, 2) == 0x1234);
    // Overflow truncation: 0x10000 -> 0
    CHECK(ParseValueToBytes(ValueType::Word, "10000", buf, 2, true));
    CHECK(ReadLE(buf, 2) == 0);

    // ---- Dword ----
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Dword, "4294967295", buf, 4, false));
    CHECK(ReadLE(buf, 4) == 0xFFFFFFFFull);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Dword, "DEADBEEF", buf, 4, true));
    CHECK(ReadLE(buf, 4) == 0xDEADBEEFull);

    // ---- Qword ----
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Qword, "18446744073709551615", buf, 8, false));
    CHECK(ReadLE(buf, 8) == 0xFFFFFFFFFFFFFFFFull);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Qword, "100000000", buf, 8, true)); // hex
    CHECK(ReadLE(buf, 8) == 0x100000000ull);
    // Value above 2^53 round-trips exactly (no double precision loss)
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Qword, "9007199254740993", buf, 8, false));
    CHECK(ReadLE(buf, 8) == 9007199254740993ull);

    // ---- Float32 ----
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Float32, "1.5", buf, 4, false));
    {
        float f; memcpy(&f, buf, 4);
        CHECK(f == 1.5f);
    }

    // ---- Float64 ----
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Float64, "2.25", buf, 8, false));
    {
        double d; memcpy(&d, buf, 8);
        CHECK(d == 2.25);
    }

    // ---- Failure cases ----
    CHECK(!ParseValueToBytes(ValueType::Dword, "notanumber", buf, 4, false));
    CHECK(!ParseValueToBytes(ValueType::Float32, "xyz", buf, 4, false));
    // String/AOB are not numeric and must be rejected here
    CHECK(!ParseValueToBytes(ValueType::String, "abc", buf, 4, false));
    CHECK(!ParseValueToBytes(ValueType::AOB, "90 90", buf, 4, false));

    // ---- Edge cases ----
    // Empty string: std::stoul throws std::invalid_argument -> returns false.
    CHECK(!ParseValueToBytes(ValueType::Byte, "", buf, 1, false));
    CHECK(!ParseValueToBytes(ValueType::Word, "", buf, 2, false));
    CHECK(!ParseValueToBytes(ValueType::Dword, "", buf, 4, false));
    CHECK(!ParseValueToBytes(ValueType::Qword, "", buf, 8, false));
    // Empty string for float types (stof/stod also throw) -> false.
    CHECK(!ParseValueToBytes(ValueType::Float32, "", buf, 4, false));
    CHECK(!ParseValueToBytes(ValueType::Float64, "", buf, 8, false));

    // Whitespace-only: std::stoul finds no digits after skipping ws -> throws
    // std::invalid_argument -> returns false.
    CHECK(!ParseValueToBytes(ValueType::Dword, "   ", buf, 4, false));
    CHECK(!ParseValueToBytes(ValueType::Float32, "   ", buf, 4, false));

    // Leading "0x" prefix with hex=true: base 16 strtoul-style parsing accepts
    // the "0x" prefix, so "0x1A" -> 0x1A (26).
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Dword, "0x1A", buf, 4, true));
    CHECK(ReadLE(buf, 4) == 0x1Aull);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Word, "0xBEEF", buf, 2, true));
    CHECK(ReadLE(buf, 2) == 0xBEEFull);

    // Negative decimal input is parsed with the signed counterpart (std::stoll)
    // and reinterpreted into the fixed-width unsigned target via two's-complement
    // truncation of the low N bytes. "-1" therefore yields all-ones at EVERY
    // width, consistently (independent of the platform's unsigned-long size).
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Byte, "-1", buf, 1, false));
    CHECK(buf[0] == 0xFF);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Word, "-1", buf, 2, false));
    CHECK(ReadLE(buf, 2) == 0xFFFFull);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Dword, "-1", buf, 4, false));
    CHECK(ReadLE(buf, 4) == 0xFFFFFFFFull);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Qword, "-1", buf, 8, false));
    CHECK(ReadLE(buf, 8) == 0xFFFFFFFFFFFFFFFFull);

    // Byte signed boundaries: -128 -> 0x80, 127 -> 0x7F, -127 -> 0x81.
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Byte, "-128", buf, 1, false));
    CHECK(buf[0] == 0x80);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Byte, "127", buf, 1, false));
    CHECK(buf[0] == 0x7F);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Byte, "-127", buf, 1, false));
    CHECK(buf[0] == 0x81);

    // Word signed boundary: -32768 -> 0x8000.
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Word, "-32768", buf, 2, false));
    CHECK(ReadLE(buf, 2) == 0x8000ull);

    // Dword negative: -256 -> 0xFFFFFF00 (two's complement of 256).
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Dword, "-256", buf, 4, false));
    CHECK(ReadLE(buf, 4) == 0xFFFFFF00ull);

    // Negative Qword: -2 -> 0xFFFFFFFFFFFFFFFE, and INT64_MIN -> 0x8000...0000.
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Qword, "-2", buf, 8, false));
    CHECK(ReadLE(buf, 8) == 0xFFFFFFFFFFFFFFFEull);
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Qword, "-9223372036854775808", buf, 8, false));
    CHECK(ReadLE(buf, 8) == 0x8000000000000000ull);

    // Leading whitespace before the sign is tolerated (isspace skip).
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Dword, "  -1", buf, 4, false));
    CHECK(ReadLE(buf, 4) == 0xFFFFFFFFull);

    // A huge negative beyond int64 range: std::stoll throws std::out_of_range,
    // caught by the existing handler -> returns false (no crash).
    CHECK(!ParseValueToBytes(ValueType::Qword, "-99999999999999999999999", buf, 8, false));

    // Large positive Qword above INT64_MAX must still parse via the unsigned
    // path (not forced through signed stoll). 0xFFFFFFFFFFFFFFFF was already
    // checked above; re-confirm a value just over INT64_MAX round-trips.
    memset(buf, 0, sizeof(buf));
    CHECK(ParseValueToBytes(ValueType::Qword, "9223372036854775809", buf, 8, false));
    CHECK(ReadLE(buf, 8) == 9223372036854775809ull);
}

// ============================================================
// Address string parsing (backs ProcessManager::ParseAddressString)
// ============================================================
static void test_address_parsing() {
    // Plain hex, no 0x prefix
    CHECK(ParsePlainHexAddress("00400000") == 0x00400000ull);
    // Plain hex, with 0x prefix
    CHECK(ParsePlainHexAddress("0x400000") == 0x400000ull);
    // Larger 64-bit value
    CHECK(ParsePlainHexAddress("7FF612340000") == 0x7FF612340000ull);

    // module+offset splitting.
    // Cheat-Engine convention: module offsets are hexadecimal. A bare offset
    // is parsed as hex, and an explicit "0x" prefix is also honored.
    std::string mod; uintptr_t off = 0;
    CHECK(SplitModuleOffset("game.exe+1A2B", mod, off));
    CHECK(mod == "game.exe");
    CHECK(off == 0x1A2B);   // bare offset parsed as hex

    // With an explicit 0x prefix the offset is parsed as hex.
    mod.clear(); off = 0;
    CHECK(SplitModuleOffset("game.exe+0x1A2B", mod, off));
    CHECK(mod == "game.exe");
    CHECK(off == 0x1A2B);   // 0x prefix -> parsed as hex

    // Bare offset parsed as hex (4096 hex == 0x4096)
    mod.clear(); off = 0;
    CHECK(SplitModuleOffset("mod.dll+4096", mod, off));
    CHECK(mod == "mod.dll");
    CHECK(off == 0x4096);

    // Trailing whitespace on module name and leading whitespace on offset trimmed
    mod.clear(); off = 0;
    CHECK(SplitModuleOffset("kernel32.dll +  0x10", mod, off));
    CHECK(mod == "kernel32.dll");
    CHECK(off == 0x10);

    // No '+' -> not module-relative
    mod = "SENTINEL"; off = 999;
    CHECK(!SplitModuleOffset("00400000", mod, off));
    CHECK(mod == "SENTINEL");   // outputs untouched on false

    // ---- Edge cases ----
    // Empty module name before '+': find('+')==0, so module is "" and the
    // rest is parsed as a hex offset. Returns true (a '+' is present).
    mod = "SENTINEL"; off = 0;
    CHECK(SplitModuleOffset("+1A2B", mod, off));
    CHECK(mod == "");           // empty module name
    CHECK(off == 0x1A2Bull);    // offset parsed as hex

    // Multiple '+': split happens on the FIRST '+' (std::string::find).
    // offsetStr becomes "1A+2B"; strtoull(base 16) parses the leading hex
    // run "1A" and stops at the second '+', yielding 0x1A.
    mod.clear(); off = 0;
    CHECK(SplitModuleOffset("game.exe+1A+2B", mod, off));
    CHECK(mod == "game.exe");   // everything before the first '+'
    CHECK(off == 0x1Aull);      // strtoull stops at the second '+'

    // Trailing '+' with no offset: offsetStr is empty, strtoull("") -> 0.
    // Still returns true because a '+' is present.
    mod.clear(); off = 999;
    CHECK(SplitModuleOffset("game.exe+", mod, off));
    CHECK(mod == "game.exe");
    CHECK(off == 0ull);         // empty offset -> 0
}

// ============================================================
// ParsePlainHexAddress edge cases
// ============================================================
static void test_plain_hex_edge() {
    // Empty string: strtoull("") has no digits to convert -> returns 0.
    CHECK(ParsePlainHexAddress("") == 0ull);
    // Non-hex garbage: strtoull("zzz", base 16) converts nothing -> returns 0.
    CHECK(ParsePlainHexAddress("zzz") == 0ull);
    // Partial hex: leading hex run is consumed until the first non-hex char.
    // "1Fzz" -> 0x1F (strtoull stops at 'z').
    CHECK(ParsePlainHexAddress("1Fzz") == 0x1Full);
    // Whitespace-only -> no digits -> 0.
    CHECK(ParsePlainHexAddress("   ") == 0ull);
}

// ============================================================
// ComputeScanChunks (backs the chunked large-region scan)
// ============================================================

// Verify that a chunk layout covers every matchable element offset in the
// region exactly once, that no chunk emits past what it can fully read, and
// that read/emit stay within bounds.
static void verify_chunk_coverage(size_t regionSize, size_t chunkSize, size_t elemLen) {
    auto chunks = ComputeScanChunks(regionSize, chunkSize, elemLen);

    size_t expectedEmit = (regionSize >= elemLen) ? (regionSize - elemLen + 1) : 0;
    if (expectedEmit == 0) {
        CHECK(chunks.empty());
        return;
    }

    // Reconstruct the set of absolute element offsets emitted, checking for
    // duplicates and gaps.
    std::vector<char> covered(expectedEmit, 0);
    size_t totalEmit = 0;
    for (auto& c : chunks) {
        // Chunk must stay within the region.
        CHECK(c.offset + c.readLen <= regionSize);
        // A chunk must be able to fully read every element it emits.
        CHECK(c.readLen >= elemLen);
        size_t readable = c.readLen - elemLen + 1;
        CHECK(c.emitLen <= readable);
        // No chunk larger than chunkSize.
        CHECK(c.readLen <= chunkSize);

        for (size_t k = 0; k < c.emitLen; k++) {
            size_t absOff = c.offset + k;
            CHECK(absOff < expectedEmit);   // in matchable range
            if (absOff < expectedEmit) {
                CHECK(covered[absOff] == 0); // emitted exactly once (no dupes)
                covered[absOff] = 1;
            }
        }
        totalEmit += c.emitLen;
    }

    CHECK(totalEmit == expectedEmit);
    for (size_t i = 0; i < expectedEmit; i++) CHECK(covered[i] == 1); // no gaps
}

static void test_compute_scan_chunks() {
    // Degenerate inputs -> empty layout.
    CHECK(ComputeScanChunks(0, 64, 4).empty());
    CHECK(ComputeScanChunks(100, 0, 4).empty());
    CHECK(ComputeScanChunks(100, 64, 0).empty());
    CHECK(ComputeScanChunks(3, 64, 4).empty());       // region smaller than element
    CHECK(ComputeScanChunks(100, 2, 4).empty());      // chunk smaller than element

    // Region fits in a single chunk.
    {
        auto c = ComputeScanChunks(1000, 4096, 4);
        CHECK(c.size() == 1);
        CHECK(c[0].offset == 0);
        CHECK(c[0].readLen == 1000);
        CHECK(c[0].emitLen == 1000 - 4 + 1);
    }

    // The headline case from the task: a 300MB region with a 64MB chunk and a
    // 4-byte value must produce exactly 5 chunks, and a value straddling a
    // 64MB boundary must be emitted by exactly one chunk (and be fully
    // readable there). Coverage/no-dupe over the whole 300MB range is verified
    // separately via scaled analogues below to avoid multi-hundred-MB test
    // allocations — the chunk math is scale-independent.
    {
        const size_t MB = 1024 * 1024;
        auto c = ComputeScanChunks(300 * MB, 64 * MB, 4);
        CHECK(c.size() == 5);

        // Each internal 64MB boundary must be crossed by an element that lands
        // in exactly one chunk's emit range and is fully readable there.
        for (int b = 1; b <= 4; b++) {
            size_t straddle = (size_t)b * 64 * MB - 2; // element crosses the edge
            int hits = 0;
            for (auto& ch : c) {
                if (straddle >= ch.offset && straddle < ch.offset + ch.emitLen) {
                    hits++;
                    CHECK(straddle - ch.offset + 4 <= ch.readLen); // fully readable
                }
            }
            CHECK(hits == 1); // emitted exactly once
        }
    }

    // Scaled analogues of the large-region layout (chunkSize=64) plus small
    // sizes and element widths, including exact multiples and off-by-one
    // boundaries, all verified for exact single coverage with no gaps.
    verify_chunk_coverage(300, 64, 4);  // scaled 300MB/64MB/4 -> 5 chunks
    verify_chunk_coverage(64, 64, 4);   // exactly one chunk
    verify_chunk_coverage(65, 64, 4);   // spills one element past a chunk
    verify_chunk_coverage(128, 64, 8);
    verify_chunk_coverage(128, 64, 1);
    verify_chunk_coverage(100, 16, 4);
    verify_chunk_coverage(100, 16, 1);
    verify_chunk_coverage(17, 16, 4);   // spills just past one chunk
    verify_chunk_coverage(33, 16, 4);
    verify_chunk_coverage(16, 16, 16);  // element == chunk == region
}

// ============================================================
// Settings::Load / Save (settings.h)
//
// Pure, header-only INI-style persistence: auto_attach (string, CR/LF
// stripped), font_size (atoi, clamped 6..20), debugger_type (atoi,
// clamped 0..2). Defaults: autoAttach="", fontSize=9, debuggerType=2.
// ============================================================

// Write raw bytes to a file (used to craft malformed / hand-authored INI).
static void write_file(const char* path, const char* contents) {
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fwrite(contents, 1, std::strlen(contents), f);
        std::fclose(f);
    }
}

static void test_settings() {
    const char* kPath = "mwe_test_settings.ini";
    const char* kMissing = "nonexistent_xyz.ini";

    // Make sure a stale file from a previous run can't taint the missing-file
    // check below.
    std::remove(kMissing);

    // ---- Defaults ----
    // Confirm the documented defaults before anything else so the missing-file
    // assertions below rest on verified ground truth.
    {
        Settings s;
        CHECK(s.autoAttach[0] == '\0');   // empty by default
        CHECK(s.fontSize == 9);
        CHECK(s.debuggerType == 2);
    }

    // ---- Round-trip Save -> Load ----
    {
        Settings out;
        std::strcpy(out.autoAttach, "game.exe");
        out.fontSize = 14;
        out.debuggerType = 1;
        out.Save(kPath);

        Settings in;
        in.Load(kPath);
        CHECK(std::strcmp(in.autoAttach, "game.exe") == 0);
        CHECK(in.fontSize == 14);
        CHECK(in.debuggerType == 1);
    }

    // ---- Missing file leaves defaults untouched ----
    {
        Settings s;
        s.Load(kMissing);   // fopen fails -> early return, no mutation
        CHECK(s.autoAttach[0] == '\0');
        CHECK(s.fontSize == 9);
        CHECK(s.debuggerType == 2);
    }

    // ---- Clamping: font_size below the floor -> 6 ----
    {
        write_file(kPath, "font_size=1\n");
        Settings s;
        s.Load(kPath);
        CHECK(s.fontSize == 6);
    }

    // ---- Clamping: font_size above the ceiling -> 20 ----
    {
        write_file(kPath, "font_size=99\n");
        Settings s;
        s.Load(kPath);
        CHECK(s.fontSize == 20);
    }

    // ---- Clamping: debugger_type below 0 -> 0 ----
    {
        write_file(kPath, "debugger_type=-5\n");
        Settings s;
        s.Load(kPath);
        CHECK(s.debuggerType == 0);
    }

    // ---- Clamping: debugger_type above 2 -> 2 ----
    {
        write_file(kPath, "debugger_type=7\n");
        Settings s;
        s.Load(kPath);
        CHECK(s.debuggerType == 2);
    }

    // ---- Malformed font_size: atoi("abc")==0 -> clamped up to floor 6 ----
    {
        write_file(kPath, "font_size=abc\n");
        Settings s;
        s.Load(kPath);
        CHECK(s.fontSize == 6);
    }

    // ---- CR/LF stripping on auto_attach ----
    // A CRLF-terminated value must load with no trailing control characters.
    {
        write_file(kPath, "auto_attach=test.exe\r\n");
        Settings s;
        s.Load(kPath);
        CHECK(std::strcmp(s.autoAttach, "test.exe") == 0);
        size_t n = std::strlen(s.autoAttach);
        CHECK(n == 8);                       // exactly "test.exe"
        CHECK(s.autoAttach[n] == '\0');      // NUL-terminated at the boundary
    }

    // Clean up the temp file.
    std::remove(kPath);
}

int main() {
    test_parse_aob();
    test_parse_value_to_bytes();
    test_address_parsing();
    test_plain_hex_edge();
    test_compute_scan_chunks();
    test_settings();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("TESTS FAILED\n");
    return 1;
}
