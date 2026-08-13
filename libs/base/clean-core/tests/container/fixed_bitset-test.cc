#include <clean-core/container/fixed_bitset.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

#include <type_traits>

using namespace cc::primitive_defines;

// Packing: the word type is the smallest that covers N, so a small bit set costs what its bits cost.
static_assert(sizeof(cc::fixed_bitset<0>) == 1, "N == 0 still holds one padding word");
static_assert(sizeof(cc::fixed_bitset<1>) == 1);
static_assert(sizeof(cc::fixed_bitset<8>) == 1);
static_assert(sizeof(cc::fixed_bitset<9>) == 2);
static_assert(sizeof(cc::fixed_bitset<32>) == 4);
static_assert(sizeof(cc::fixed_bitset<33>) == 8);
static_assert(sizeof(cc::fixed_bitset<64>) == 8);
static_assert(sizeof(cc::fixed_bitset<65>) == 16, "past 64 bits the storage is an array of u64");
static_assert(sizeof(cc::fixed_bitset<128>) == 16);
static_assert(sizeof(cc::fixed_bitset<129>) == 24);

static_assert(std::is_trivially_copyable_v<cc::fixed_bitset<100>>);

namespace
{
/// Everything below runs in a constant expression, which is what pins fixed_bitset as usable for compile-time tables.
consteval bool constexpr_roundtrip()
{
    auto bs = cc::fixed_bitset<70>();
    bs.set(3);
    bs.set(69);
    bs.set(64, true);
    bs.set(3, false);
    return bs.set_bit_count() == 2 && bs.find_first_set() == 64 && bs.find_last_set() == 69 && !bs.is_set(3);
}
static_assert(constexpr_roundtrip());

/// The structural-type property: a fixed_bitset can be a non-type template parameter.
template <cc::fixed_bitset<8> Mask>
constexpr isize masked_count()
{
    return Mask.set_bit_count();
}
static_assert(masked_count<cc::fixed_bitset<8>::create_from_u64(0b1011)>() == 3);
static_assert(cc::fixed_bitset<8>::create_from_u64(0b1011) == cc::fixed_bitset<8>::create_from_u64(0b1011));

/// A reference bit set to check the word-wise implementations against.
template <isize N>
cc::vector<bool> reference_of(cc::fixed_bitset<N> const& bs)
{
    auto r = cc::vector<bool>();
    for (isize i = 0; i < N; ++i)
        r.push_back(bs.is_set(i));
    return r;
}

/// A deterministic, word-boundary-crossing pattern.
template <isize N>
cc::fixed_bitset<N> make_pattern(u64 seed)
{
    auto bs = cc::fixed_bitset<N>();
    for (isize i = 0; i < N; ++i)
    {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        if ((seed >> 33) % 3 == 0)
            bs.set(i);
    }
    return bs;
}
} // namespace

TEST("fixed_bitset - default is all zero")
{
    auto const bs = cc::fixed_bitset<70>();
    CHECK(bs.size() == 70);
    CHECK(bs.none_set());
    CHECK(!bs.any_set());
    CHECK(!bs.all_set());
    CHECK(bs.set_bit_count() == 0);
    CHECK(bs.unset_bit_count() == 70);
    CHECK(bs.find_first_set() == -1);
    CHECK(bs.find_last_set() == -1);
    CHECK(bs.find_first_unset() == 0);
    CHECK(bs.find_last_unset() == 69);
}

TEST("fixed_bitset - N == 0 is permanently empty")
{
    auto bs = cc::fixed_bitset<0>();
    CHECK(bs.size() == 0);
    CHECK(bs.word_count() == 0);
    CHECK(bs.none_set());
    CHECK(bs.all_set()); // vacuously
    CHECK(bs.find_first_set() == -1);
    CHECK(bs.find_last_set() == -1);
    CHECK(bs.find_first_unset() == -1);
    bs.set_all();
    bs.toggle_all();
    CHECK(bs.set_bit_count() == 0);

    auto count = 0;
    for ([[maybe_unused]] auto i : bs.set_indices())
        ++count;
    CHECK(count == 0);
}

TEST("fixed_bitset - single bit mutation")
{
    auto bs = cc::fixed_bitset<100>();
    bs.set(0);
    bs.set(63);
    bs.set(64);
    bs.set(99);
    CHECK(bs.set_bit_count() == 4);
    CHECK(bs.is_set(63));
    CHECK(bs.is_set(64));
    CHECK(!bs.is_set(65));

    bs.unset(64);
    CHECK(!bs.is_set(64));
    bs.toggle(64);
    CHECK(bs.is_set(64));
    bs.toggle(64);
    CHECK(!bs.is_set(64));

    bs.set(7, true);
    CHECK(bs.is_set(7));
    bs.set(7, false);
    CHECK(!bs.is_set(7));
}

TEST("fixed_bitset - operator[] proxy")
{
    auto bs = cc::fixed_bitset<70>();
    bs[5] = true;
    CHECK(bs.is_set(5));
    CHECK(bs[5]);
    CHECK(!bs[6]);

    bs[6] = bs[5]; // assigns the value, not the reference
    CHECK(bs[6]);
    bs[5] = false;
    CHECK(bs[6]); // 6 is unaffected

    auto const& cbs = bs;
    CHECK(cbs[6]);
    CHECK(!cbs[5]);
}

TEST("fixed_bitset - set_all and toggle_all keep the tail zero")
{
    // The tail bits above N must never leak into a count or a search, whatever wrote whole words.
    auto bs = cc::fixed_bitset<70>();
    bs.set_all();
    CHECK(bs.all_set());
    CHECK(bs.set_bit_count() == 70);
    CHECK(bs.find_last_set() == 69);
    CHECK(bs.find_first_unset() == -1);

    bs.toggle_all();
    CHECK(bs.none_set());
    CHECK(bs.set_bit_count() == 0);

    bs.set(69);
    bs.toggle_all();
    CHECK(bs.set_bit_count() == 69);
    CHECK(bs.find_last_set() == 68);

    bs.set_all(false);
    CHECK(bs.none_set());
    bs.set_all(true);
    CHECK(bs.all_set());
}

TEST("fixed_bitset - ranges across word boundaries")
{
    auto bs = cc::fixed_bitset<200>();

    bs.set_range(60, 10); // spans the 64-bit boundary
    CHECK(bs.set_bit_count() == 10);
    CHECK(bs.find_first_set() == 60);
    CHECK(bs.find_last_set() == 69);

    bs.set_range(3, 4); // fully inside one word
    CHECK(bs.set_bit_count() == 14);

    bs.unset_range(62, 4);
    CHECK(bs.set_bit_count() == 10);
    CHECK(!bs.is_set(63));
    CHECK(!bs.is_set(64));
    CHECK(bs.is_set(66));

    bs.unset_all();
    bs.toggle_range(0, 200); // the whole thing
    CHECK(bs.all_set());
    bs.toggle_range(0, 200);
    CHECK(bs.none_set());

    bs.set_range(10, 0); // empty range is a no-op
    CHECK(bs.none_set());

    bs.set_range(5, 3, true);
    CHECK(bs.set_bit_count() == 3);
    bs.set_range(5, 3, false);
    CHECK(bs.none_set());
}

TEST("fixed_bitset - find over word boundaries")
{
    auto bs = cc::fixed_bitset<200>();
    bs.set(0);
    bs.set(64);
    bs.set(130);

    CHECK(bs.find_first_set() == 0);
    CHECK(bs.find_first_set(1) == 64);
    CHECK(bs.find_first_set(64) == 64);
    CHECK(bs.find_first_set(65) == 130);
    CHECK(bs.find_first_set(131) == -1);
    CHECK(bs.find_first_set(200) == -1);

    CHECK(bs.find_last_set() == 130);
    CHECK(bs.find_last_set(130) == 64);
    CHECK(bs.find_last_set(64) == 0);
    CHECK(bs.find_last_set(0) == -1);

    bs.set_all();
    bs.unset(0);
    bs.unset(64);
    bs.unset(199);
    CHECK(bs.find_first_unset() == 0);
    CHECK(bs.find_first_unset(1) == 64);
    CHECK(bs.find_first_unset(65) == 199);
    CHECK(bs.find_last_unset() == 199);
    CHECK(bs.find_last_unset(199) == 64);
}

TEST("fixed_bitset - set_indices and unset_indices match a per-index loop")
{
    auto const bs = make_pattern<200>(12345);
    auto const reference = reference_of(bs);

    auto expected_set = cc::vector<isize>();
    auto expected_unset = cc::vector<isize>();
    for (isize i = 0; i < 200; ++i)
        (reference[i] ? expected_set : expected_unset).push_back(i);

    auto actual_set = cc::vector<isize>();
    for (auto i : bs.set_indices())
        actual_set.push_back(i);
    auto actual_unset = cc::vector<isize>();
    for (auto i : bs.unset_indices())
        actual_unset.push_back(i);

    CHECK(actual_set.size() == expected_set.size());
    for (isize i = 0; i < expected_set.size(); ++i)
        CHECK(actual_set[i] == expected_set[i]);

    CHECK(actual_unset.size() == expected_unset.size());
    for (isize i = 0; i < expected_unset.size(); ++i)
        CHECK(actual_unset[i] == expected_unset[i]);

    // The unset iteration must stop at size(), not at the word boundary above it.
    CHECK(actual_set.size() + actual_unset.size() == 200);
}

TEST("fixed_bitset - unset_indices of an all-set bitset is empty")
{
    auto const bs = cc::fixed_bitset<70>::create_all_set();
    auto count = 0;
    for ([[maybe_unused]] auto i : bs.unset_indices())
        ++count;
    CHECK(count == 0);

    auto set_count = 0;
    for ([[maybe_unused]] auto i : bs.set_indices())
        ++set_count;
    CHECK(set_count == 70);
}

TEST("fixed_bitset - whole-set operations")
{
    auto const a = make_pattern<130>(1);
    auto const b = make_pattern<130>(2);
    auto const ra = reference_of(a);
    auto const rb = reference_of(b);

    auto expected_union = 0;
    auto expected_intersection = 0;
    auto expected_difference = 0;
    auto expected_symmetric = 0;
    for (isize i = 0; i < 130; ++i)
    {
        expected_union += ra[i] || rb[i] ? 1 : 0;
        expected_intersection += ra[i] && rb[i] ? 1 : 0;
        expected_difference += ra[i] && !rb[i] ? 1 : 0;
        expected_symmetric += ra[i] != rb[i] ? 1 : 0;
    }

    CHECK(cc::fixed_bitset<130>::create_union_of(a, b).set_bit_count() == expected_union);
    CHECK(cc::fixed_bitset<130>::create_intersection_of(a, b).set_bit_count() == expected_intersection);
    CHECK(cc::fixed_bitset<130>::create_difference_of(a, b).set_bit_count() == expected_difference);
    CHECK(cc::fixed_bitset<130>::create_symmetric_difference_of(a, b).set_bit_count() == expected_symmetric);
    CHECK(a.intersection_bit_count(b) == expected_intersection);

    auto m = a;
    m.set_all_of(b);
    CHECK(m == cc::fixed_bitset<130>::create_union_of(a, b));
    m = a;
    m.retain_all_of(b);
    CHECK(m == cc::fixed_bitset<130>::create_intersection_of(a, b));
    m = a;
    m.unset_all_of(b);
    CHECK(m == cc::fixed_bitset<130>::create_difference_of(a, b));
    m = a;
    m.toggle_all_of(b);
    CHECK(m == cc::fixed_bitset<130>::create_symmetric_difference_of(a, b));
}

TEST("fixed_bitset - subset and disjointness")
{
    auto whole = cc::fixed_bitset<70>();
    whole.set_range(0, 40);
    auto part = cc::fixed_bitset<70>();
    part.set_range(10, 5);
    auto other = cc::fixed_bitset<70>();
    other.set_range(50, 5);

    CHECK(whole.has_all(part));
    CHECK(!part.has_all(whole));
    CHECK(whole.has_any(part));
    CHECK(!whole.has_any(other));
    CHECK(whole.is_disjoint(other));
    CHECK(!whole.is_disjoint(part));
    CHECK(whole.intersection_bit_count(part) == 5);
    CHECK(whole.intersection_bit_count(other) == 0);

    auto const empty = cc::fixed_bitset<70>();
    CHECK(whole.has_all(empty)); // the empty set is a subset of everything
    CHECK(whole.is_disjoint(empty));
}

TEST("fixed_bitset - u64 conversion")
{
    CHECK(cc::fixed_bitset<8>::create_from_u64(0b1011).to_u64() == 0b1011);
    CHECK(cc::fixed_bitset<64>::create_from_u64(~u64(0)).to_u64() == ~u64(0));
    CHECK(cc::fixed_bitset<0>().to_u64() == 0);

    auto bs = cc::fixed_bitset<8>();
    bs.set(0);
    bs.set(7);
    CHECK(bs.to_u64() == 0b1000'0001);
}

TEST("fixed_bitset - equality and hashing")
{
    auto a = cc::fixed_bitset<70>();
    auto b = cc::fixed_bitset<70>();
    CHECK(a == b);
    CHECK(hash(a) == hash(b));

    a.set(69);
    CHECK(a != b);
    b.set(69);
    CHECK(a == b);
    CHECK(hash(a) == hash(b));
}

TEST("fixed_bitset - words_span")
{
    auto bs = cc::fixed_bitset<70>();
    bs.set(0);
    bs.set(64);
    auto const words = bs.words_span();
    CHECK(words.size() == 2);
    CHECK(words[0] == 1);
    CHECK(words[1] == 1);
}
