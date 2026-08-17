#include <clean-core/common/endian.hh>
#include <clean-core/container/span.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
// The reference pattern every load test reads back.
// Each byte is distinct, so a wrong shift shows up as a wrong nibble rather than as a plausible number.
constexpr byte pattern[8] = {
    byte(0x11), byte(0x22), byte(0x33), byte(0x44), byte(0x55), byte(0x66), byte(0x77), byte(0x88),
};

constexpr cc::span<byte const> pattern_span()
{
    return cc::span<byte const>(pattern, 8);
}

// Round-trips through a fresh buffer at a non-zero offset, which is what a durable format actually does with these.
template <class T>
constexpr bool store_load_le(T v)
{
    byte buffer[16] = {};
    auto const bytes = cc::span<byte>(buffer, 16);
    cc::store_bytes_le<T>(bytes, 3, v);
    return cc::load_bytes_le<T>(bytes, 3) == v;
}

template <class T>
constexpr bool store_load_be(T v)
{
    byte buffer[16] = {};
    auto const bytes = cc::span<byte>(buffer, 16);
    cc::store_bytes_be<T>(bytes, 3, v);
    return cc::load_bytes_be<T>(bytes, 3) == v;
}
} // namespace

TEST("cc::endian - load little-endian")
{
    CHECK(cc::load_bytes_le<u8>(pattern_span()) == 0x11);
    CHECK(cc::load_bytes_le<u16>(pattern_span()) == 0x2211);
    CHECK(cc::load_bytes_le<u32>(pattern_span()) == 0x44332211u);
    CHECK(cc::load_bytes_le<u64>(pattern_span()) == 0x8877665544332211ull);
}

TEST("cc::endian - load big-endian")
{
    CHECK(cc::load_bytes_be<u8>(pattern_span()) == 0x11);
    CHECK(cc::load_bytes_be<u16>(pattern_span()) == 0x1122);
    CHECK(cc::load_bytes_be<u32>(pattern_span()) == 0x11223344u);
    CHECK(cc::load_bytes_be<u64>(pattern_span()) == 0x1122334455667788ull);
}

TEST("cc::endian - the offset indexes bytes, not elements")
{
    CHECK(cc::load_bytes_le<u16>(pattern_span(), 1) == 0x3322);
    CHECK(cc::load_bytes_be<u16>(pattern_span(), 1) == 0x2233);

    // the last offset that still fits; one past it asserts
    CHECK(cc::load_bytes_le<u32>(pattern_span(), 4) == 0x88776655u);
    CHECK(cc::load_bytes_be<u32>(pattern_span(), 4) == 0x55667788u);
}

TEST("cc::endian - the two orders are byte reversals of each other")
{
    byte buffer[8] = {};
    auto const bytes = cc::span<byte>(buffer, 8);

    cc::store_bytes_le<u64>(bytes, 0, 0x0102030405060708ull);
    CHECK(cc::load_bytes_be<u64>(bytes) == 0x0807060504030201ull);
}

TEST("cc::endian - store then load round-trips")
{
    CHECK(store_load_le<u8>(0xABu));
    CHECK(store_load_le<i16>(-12345));
    CHECK(store_load_le<u32>(0xDEADBEEFu));
    CHECK(store_load_le<i64>(-9007199254740993ll));

    CHECK(store_load_be<u8>(0xABu));
    CHECK(store_load_be<i16>(-12345));
    CHECK(store_load_be<u32>(0xDEADBEEFu));
    CHECK(store_load_be<i64>(-9007199254740993ll));
}

TEST("cc::endian - integer extremes survive")
{
    CHECK(store_load_le<i64>(i64(0x8000000000000000ull)));
    CHECK(store_load_le<i64>(0x7FFFFFFFFFFFFFFFll));
    CHECK(store_load_le<u64>(0xFFFFFFFFFFFFFFFFull));

    CHECK(store_load_be<i64>(i64(0x8000000000000000ull)));
    CHECK(store_load_be<i64>(0x7FFFFFFFFFFFFFFFll));
    CHECK(store_load_be<u64>(0xFFFFFFFFFFFFFFFFull));
}

TEST("cc::endian - floating point goes through its bit pattern")
{
    CHECK(store_load_le<f32>(1.5f));
    CHECK(store_load_le<f64>(-0.125));
    CHECK(store_load_be<f64>(3.141592653589793));

    // an f64 is stored as its binary64 bit pattern, so the little-endian bytes are that pattern's bytes
    byte buffer[8] = {};
    auto const bytes = cc::span<byte>(buffer, 8);
    cc::store_bytes_le<f64>(bytes, 0, 1.0);
    CHECK(cc::load_bytes_le<u64>(bytes) == 0x3FF0000000000000ull);
}

TEST("cc::endian - negative zero and NaN keep their exact bits")
{
    byte buffer[8] = {};
    auto const bytes = cc::span<byte>(buffer, 8);

    cc::store_bytes_le<f64>(bytes, 0, -0.0);
    CHECK(cc::load_bytes_le<u64>(bytes) == 0x8000000000000000ull);

    // NaN payloads survive a round-trip, which is what lets a caller keep two NaNs distinguishable
    cc::store_bytes_le<u64>(bytes, 0, 0x7FF8000000000001ull);
    auto const nan_payload = cc::load_bytes_le<f64>(bytes);
    cc::store_bytes_le<f64>(bytes, 0, nan_payload);
    CHECK(cc::load_bytes_le<u64>(bytes) == 0x7FF8000000000001ull);
}

TEST("cc::endian - everything is usable at compile time")
{
    static_assert(cc::load_bytes_le<u32>(cc::span<byte const>(pattern, 8)) == 0x44332211u);
    static_assert(cc::load_bytes_be<u32>(cc::span<byte const>(pattern, 8)) == 0x11223344u);
    static_assert(store_load_le<u64>(0xFEDCBA9876543210ull));
    static_assert(store_load_be<u64>(0xFEDCBA9876543210ull));
    SUCCEED();
}

TEST("cc::endian - reading or writing past the end asserts")
{
    CHECK_ASSERTS(cc::load_bytes_le<u64>(pattern_span(), 1));
    CHECK_ASSERTS(cc::load_bytes_be<u32>(pattern_span(), 5));
    CHECK_ASSERTS(cc::load_bytes_le<u16>(pattern_span(), -1));

    byte buffer[4] = {};
    auto const bytes = cc::span<byte>(buffer, 4);
    CHECK_ASSERTS(cc::store_bytes_le<u64>(bytes, 0, 0));
    CHECK_ASSERTS(cc::store_bytes_be<u32>(bytes, 1, 0));
}
