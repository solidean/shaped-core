#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/bench/barriers.hh>
#include <nexus/test.hh>

// The contract of the optimization barriers, which is the half that regresses.
//
// What is NOT pinned here is that they have their intended EFFECT on codegen — that a guarded loop survives the
// optimizer where an unguarded one does not.
// No portable runtime assertion says that: a timing comparison is flaky by construction, and the honest checks are the
// self-benchmarks and `dev.py assembly`, neither of which belongs in a unit test.
//
// So these pin what a wrong constraint or a wrong clobber would actually break: values coming back altered, and stores
// into a guarded buffer going missing.

using namespace cc::primitive_defines;

namespace
{
struct big
{
    u64 words[16] = {};
};

// Move-only, to pin that the guard never copies what it is handed.
struct movable
{
    int value = 0;

    movable() = default;
    explicit movable(int v) : value(v) {}
    movable(movable&&) = default;
    movable& operator=(movable&&) = default;
    movable(movable const&) = delete;
    movable& operator=(movable const&) = delete;
};
} // namespace

TEST("bench - keep is value preserving")
{
    auto const i = 42;
    CHECK(nx::bench::keep(i) == 42);

    auto d = 3.5;
    CHECK(nx::bench::keep(d) == 3.5);

    // A guard that clobbered the wrong register would corrupt the accumulator rather than fail to compile, so the
    // comparison against an unguarded run is the assertion that matters.
    auto guarded = u64(0);
    auto plain = u64(0);
    for (auto k = u64(0); k < 1000; ++k)
    {
        guarded += nx::bench::keep(k * k);
        plain += k * k;
    }
    CHECK(guarded == plain);
}

TEST("bench - keep preserves value category and constness")
{
    auto i = 7;
    auto const c = 7;

    static_assert(std::is_same_v<decltype(nx::bench::keep(i)), int&>);
    static_assert(std::is_same_v<decltype(nx::bench::keep(c)), int const&>);
    static_assert(std::is_same_v<decltype(nx::bench::keep(3)), int&&>);

    // An lvalue comes back as the same object rather than as a copy of it.
    CHECK(&nx::bench::keep(i) == &i);
}

TEST("bench - keep takes types too large for a register, and move-only ones")
{
    auto b = big{};
    b.words[15] = 99;
    CHECK(nx::bench::keep(b).words[15] == 99);

    auto m = movable(5);
    CHECK(nx::bench::keep(m).value == 5);

    auto moved = movable(nx::bench::keep(cc::move(m)));
    CHECK(moved.value == 5);
}

TEST("bench - sink over a span keeps the buffer's contents")
{
    auto buffer = cc::vector<u32>::create_defaulted(256);
    for (auto i = 0; i < 256; ++i)
        buffer[i] = u32(i * 3);

    nx::bench::sink(cc::as_bytes(buffer));

    // The memory clobber must not have been read as licence to reorder the stores away.
    CHECK(buffer[0] == 0);
    CHECK(buffer[255] == 765);
}

TEST("bench - compiler_barrier is callable and orders nothing observable")
{
    auto x = 1;
    nx::bench::compiler_barrier();
    x = 2;
    nx::bench::compiler_barrier();
    CHECK(x == 2);
}

TEST("bench - evict_data_caches accepts every size, including nonsense ones")
{
    // 0 means the whole buffer, and the first call is the one that pays for the allocation.
    nx::bench::evict_data_caches(0);

    nx::bench::evict_data_caches(1 << 20);

    // Below one cache line, and negative: neither may read out of bounds or hang.
    nx::bench::evict_data_caches(1);
    nx::bench::evict_data_caches(-1);

    // Larger than the buffer clamps rather than growing it.
    nx::bench::evict_data_caches(nx::bench::default_evict_bytes * 4);

    CHECK(true); // reaching here without a crash or a hang is the assertion
}
