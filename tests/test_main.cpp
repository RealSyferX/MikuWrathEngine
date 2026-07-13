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
// ============================================================
#include "scanner.h"
#include "parse_utils.h"
#include "types.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

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
}

int main() {
    test_parse_aob();
    test_parse_value_to_bytes();
    test_address_parsing();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("TESTS FAILED\n");
    return 1;
}
