#include <clean-core/fwd.hh>
#include <nexus/test.hh>
#include <shaped-rendering/gpu_types.hh>

#include <type_traits>

using namespace cc::primitive_defines;

TEST("sr - gpu_boolean packs a bool into one 32-bit lane")
{
    static_assert(sizeof(sr::gpu_boolean) == 4);
    static_assert(std::is_trivially_copyable_v<sr::gpu_boolean>); // it rides into a cbuffer by memcpy

    auto const t = sr::gpu_boolean(true);
    auto const f = sr::gpu_boolean();

    CHECK(t.value == 1u);
    CHECK(f.value == 0u);
    CHECK(bool(t));
    CHECK(!bool(f));
    CHECK(t == sr::gpu_boolean(true));
    CHECK(t != f);

    // A shader reads any non-zero lane as `true`, so an off-by-one bit pattern is still equal to `true` here.
    auto raw = sr::gpu_boolean();
    raw.value = 0xFFFFFFFFu;
    CHECK(bool(raw));
    CHECK(raw == t);
}

TEST("sr - gpu_boolean is a drop-in cbuffer field")
{
    // What a `*_gpu` struct looks like where it carries a flag: the lane is declared gpu_boolean and a plain bool assigns into it.
    struct constants
    {
        sr::gpu_boolean is_indexed = false;
        u32 _padding[3] = {};
    };

    static_assert(sizeof(constants) == 16);

    auto c = constants{};
    CHECK(c.is_indexed.value == 0u);

    auto const record_is_indexed = true; // what a caller has: a plain bool off its own record
    c.is_indexed = record_is_indexed;
    CHECK(c.is_indexed.value == 1u);
}
