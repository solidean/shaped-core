#include <clean-core/fwd.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/gpu_bool.hh>

#include <type_traits>

using namespace cc::primitive_defines;

TEST("slib - gpu_bool packs a bool into one 32-bit lane")
{
    static_assert(sizeof(slib::gpu_bool) == 4);
    static_assert(std::is_trivially_copyable_v<slib::gpu_bool>); // it rides into GPU memory by memcpy

    auto const t = slib::gpu_bool(true);
    auto const f = slib::gpu_bool();

    CHECK(t.value == 1u);
    CHECK(f.value == 0u);
    CHECK(bool(t));
    CHECK(!bool(f));
    CHECK(t == slib::gpu_bool(true));
    CHECK(t != f);

    // A shader reads any non-zero lane as `true`, so an off-by-one bit pattern is still equal to `true` here.
    auto raw = slib::gpu_bool();
    raw.value = 0xFFFFFFFFu;
    CHECK(bool(raw));
    CHECK(raw == t);
}

TEST("slib - gpu_bool is a drop-in field of a GPU struct")
{
    // What a GPU struct looks like where it carries a flag: the lane is declared gpu_bool and a plain bool assigns into it.
    struct constants
    {
        slib::gpu_bool is_indexed = false;
        u32 _padding[3] = {};
    };

    static_assert(sizeof(constants) == 16);

    auto c = constants{};
    CHECK(c.is_indexed.value == 0u);

    auto const record_is_indexed = true; // what a caller has: a plain bool off its own record
    c.is_indexed = record_is_indexed;
    CHECK(c.is_indexed.value == 1u);
}
