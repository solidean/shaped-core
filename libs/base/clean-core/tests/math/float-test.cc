#include <clean-core/math/float.hh>
#include <nexus/test.hh>

#include <limits>

using namespace cc::primitive_defines;

namespace
{
template <class F, class Bits>
constexpr F from_bits(Bits bits)
{
    return cc::bit_cast<F>(bits);
}
} // namespace

static_assert(cc::is_finite(1.0f) && !cc::is_nan(1.0f) && !cc::is_inf(1.0f), "classification is constexpr");
static_assert(cc::is_nan(std::numeric_limits<f64>::quiet_NaN()) && !cc::is_finite(std::numeric_limits<f64>::infinity()));

TEST("float - classification of every IEEE-754 category")
{
    SECTION("f32")
    {
        auto const max = std::numeric_limits<f32>::max();
        auto const denorm = std::numeric_limits<f32>::denorm_min();
        auto const inf = std::numeric_limits<f32>::infinity();
        auto const qnan = std::numeric_limits<f32>::quiet_NaN();
        auto const snan = std::numeric_limits<f32>::signaling_NaN();
        auto const negative_nan = from_bits<f32>(u32(0xFFC0'0000u));
        auto const smallest_payload_nan = from_bits<f32>(u32(0x7F80'0001u));

        for (auto const v : {0.0f, -0.0f, 1.0f, -1.0f, max, -max, denorm, -denorm})
        {
            CHECK(cc::is_finite(v));
            CHECK(!cc::is_nan(v));
            CHECK(!cc::is_inf(v));
        }

        for (auto const v : {inf, -inf})
        {
            CHECK(!cc::is_finite(v));
            CHECK(!cc::is_nan(v));
            CHECK(cc::is_inf(v));
        }

        for (auto const v : {qnan, snan, negative_nan, smallest_payload_nan})
        {
            CHECK(!cc::is_finite(v));
            CHECK(cc::is_nan(v));
            CHECK(!cc::is_inf(v));
        }
    }

    SECTION("f64")
    {
        auto const max = std::numeric_limits<f64>::max();
        auto const denorm = std::numeric_limits<f64>::denorm_min();
        auto const inf = std::numeric_limits<f64>::infinity();
        auto const qnan = std::numeric_limits<f64>::quiet_NaN();
        auto const snan = std::numeric_limits<f64>::signaling_NaN();
        auto const negative_nan = from_bits<f64>(u64(0xFFF8'0000'0000'0000ull));
        auto const smallest_payload_nan = from_bits<f64>(u64(0x7FF0'0000'0000'0001ull));

        for (auto const v : {0.0, -0.0, 1.0, -1.0, max, -max, denorm, -denorm})
        {
            CHECK(cc::is_finite(v));
            CHECK(!cc::is_nan(v));
            CHECK(!cc::is_inf(v));
        }

        for (auto const v : {inf, -inf})
        {
            CHECK(!cc::is_finite(v));
            CHECK(!cc::is_nan(v));
            CHECK(cc::is_inf(v));
        }

        for (auto const v : {qnan, snan, negative_nan, smallest_payload_nan})
        {
            CHECK(!cc::is_finite(v));
            CHECK(cc::is_nan(v));
            CHECK(!cc::is_inf(v));
        }
    }
}
