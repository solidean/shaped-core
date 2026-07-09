#include <clean-core/common/hash.hh>
#include <clean-core/container/span.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr u64 U64_MAX = 0xFFFFFFFFFFFFFFFFull;

enum class color : u8
{
    red = 7,
};

// A type that customizes hashing via the ADL hidden friend (the common path).
struct only_friend
{
    u64 v;
    friend u64 hash(only_friend const& x) { return x.v; }
};

// A type with BOTH a hidden friend and a trait specialization — the trait must win.
struct overridden
{
    u64 v;
    friend u64 hash(overridden const& x) { return x.v; }
};

cc::span<cc::byte const> bytes_of(char const* s, isize n)
{
    return cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(s), n);
}
} // namespace

template <>
struct cc::custom::hash_trait<overridden>
{
    [[nodiscard]] static constexpr u64 hash(overridden const& x) { return x.v + 1000; }
};

TEST("hash - built-in integers/bool/char/enum are composable identity")
{
    CHECK(cc::make_hash(u64(0)) == 0ull);
    CHECK(cc::make_hash(u64(5)) == 5ull);
    CHECK(cc::make_hash(5) == 5ull);
    CHECK(cc::make_hash(int(-1)) == 0xFFFFFFFFull); // make_unsigned<int> then zero-extend
    CHECK(cc::make_hash(i64(-1)) == U64_MAX);

    CHECK(cc::make_hash(true) == 1ull);
    CHECK(cc::make_hash(false) == 0ull);
    CHECK(cc::make_hash('A') == 65ull);
    CHECK(cc::make_hash(color::red) == 7ull);
}

TEST("hash - floats hash by bits with -0.0 collapsed")
{
    CHECK(cc::make_hash(0.0f) == cc::make_hash(-0.0f));
    CHECK(cc::make_hash(0.0) == cc::make_hash(-0.0));
    CHECK(cc::make_hash(1.0f) == 0x3F800000ull);
    CHECK(cc::make_hash(1.0) == 0x3FF0000000000000ull);
    CHECK(cc::make_hash(1.0f) != cc::make_hash(2.0f));
}

TEST("hash - pointers hash by address")
{
    int x = 0;
    CHECK(cc::make_hash(&x) == reinterpret_cast<u64>(&x));
    CHECK(cc::make_hash(nullptr) == 0ull);
}

TEST("hash - customization: hidden friend, and trait overrides friend")
{
    CHECK(cc::make_hash(only_friend{42}) == 42ull);
    CHECK(cc::make_hash(overridden{42}) == 1042ull); // trait wins over the friend's 42
}

TEST("hash - combine and variadic fold")
{
    CHECK(cc::combine_hash(1, 2) != cc::combine_hash(2, 1)); // order dependent

    // make_hash(a, b) == combine_hash(make_hash(a), make_hash(b))
    CHECK(cc::make_hash(7, 9) == cc::combine_hash(cc::make_hash(7), cc::make_hash(9)));
    // make_hash(a, b, c) folds left
    CHECK(cc::make_hash(7, 9, 11)
          == cc::combine_hash(cc::combine_hash(cc::make_hash(7), cc::make_hash(9)), cc::make_hash(11)));

    CHECK(cc::make_hash(1, 2, 3) == cc::make_hash(1, 2, 3)); // deterministic
    CHECK(cc::make_hash(1, 2) != cc::make_hash(2, 1));       // ordered
}

TEST("hash - unordered combine is commutative")
{
    auto const a = cc::make_hash_finalized(11);
    auto const b = cc::make_hash_finalized(22);
    CHECK(cc::combine_hash_unordered(a, b) == cc::combine_hash_unordered(b, a));
}

TEST("hash - finalize")
{
    CHECK(cc::make_hash_finalized(u64(5)) == cc::hash_finalize(cc::make_hash(u64(5))));
    CHECK(cc::make_hash_finalized(u64(5)) != 5ull); // identity input gets avalanged
    CHECK(cc::make_hash_finalized(u64(5)) != cc::make_hash_finalized(u64(6)));
    CHECK(cc::hash_finalize(0) == 0ull); // 0 is a fixed point of the multiply/xorshift mixer
}

TEST("hash - make_hash_of_bytes known answers (XXH3-64, v0.8.3)")
{
    cc::span<cc::byte const> const empty;
    CHECK(cc::make_hash_of_bytes(empty, 0) == 0x2d06800538d394c2ull);
    CHECK(cc::make_hash_of_bytes(bytes_of("abc", 3), 0) == 0x78af5f94892f3950ull);
    CHECK(cc::make_hash_of_bytes(bytes_of("hello world", 11), 0) == 0xd447b1ea40e6988bull);
    CHECK(cc::make_hash_of_bytes(bytes_of("hello world", 11), 1) == 0xb7aeb52a10fdaf2dull);

    // determinism + seed sensitivity
    auto const d = bytes_of("the quick brown fox", 19);
    CHECK(cc::make_hash_of_bytes(d, 0) == cc::make_hash_of_bytes(d, 0));
    CHECK(cc::make_hash_of_bytes(d, 0) != cc::make_hash_of_bytes(d, 1));
}
