#include <clean-core/container/bitset.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
/// A deterministic, word-boundary-crossing pattern, plus the per-index reference it was built from.
cc::vector<bool> make_pattern(isize bit_count, u64 seed)
{
    auto r = cc::vector<bool>();
    for (isize i = 0; i < bit_count; ++i)
    {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        r.push_back((seed >> 33) % 3 == 0);
    }
    return r;
}

cc::bitset bitset_of(cc::vector<bool> const& pattern)
{
    auto bs = cc::bitset::create_defaulted(pattern.size());
    for (isize i = 0; i < pattern.size(); ++i)
        bs.set(i, pattern[i]);
    return bs;
}
} // namespace

TEST("bitset - default is empty and allocation-free")
{
    auto const bs = cc::bitset();
    CHECK(bs.size() == 0);
    CHECK(bs.capacity() == 0);
    CHECK(bs.word_count() == 0);
    CHECK(bs.none_set());
    CHECK(bs.all_set()); // vacuously
    CHECK(bs.find_first_set() == -1);
    CHECK(bs.find_last_set() == -1);
}

TEST("bitset - factories")
{
    auto const zeros = cc::bitset::create_defaulted(70);
    CHECK(zeros.size() == 70);
    CHECK(zeros.none_set());

    auto const ones = cc::bitset::create_filled(70, true);
    CHECK(ones.size() == 70);
    CHECK(ones.all_set());
    CHECK(ones.set_bit_count() == 70);
    CHECK(ones.find_last_set() == 69);
    CHECK(ones.find_first_unset() == -1); // the tail above 70 must not read as unset

    auto const reserved = cc::bitset::create_with_capacity(200);
    CHECK(reserved.size() == 0);
    CHECK(reserved.capacity() >= 200);
}

TEST("bitset - resize preserves content and keeps the tail zero")
{
    auto bs = cc::bitset::create_defaulted(10);
    bs.set(3);
    bs.set(9);

    bs.resize_to_filled(100, true);
    CHECK(bs.size() == 100);
    CHECK(bs.is_set(3));
    CHECK(bs.is_set(9));
    CHECK(!bs.is_set(0));
    CHECK(bs.set_bit_count() == 2 + 90);

    // Shrinking then growing again must not resurrect the dropped bits.
    bs.resize_to_filled(20);
    CHECK(bs.size() == 20);
    CHECK(bs.set_bit_count() == 2 + 10);
    bs.resize_to_filled(100);
    CHECK(bs.size() == 100);
    CHECK(bs.set_bit_count() == 2 + 10);
    CHECK(bs.find_last_set() == 19);

    bs.resize_to_filled(0);
    CHECK(bs.size() == 0);
    CHECK(bs.none_set());
    bs.resize_to_filled(200);
    CHECK(bs.none_set());
}

TEST("bitset - push_back and pop_back round-trip")
{
    auto const pattern = make_pattern(300, 7);

    auto bs = cc::bitset();
    for (auto const b : pattern)
        bs.push_back(b);

    CHECK(bs.size() == 300);
    for (isize i = 0; i < 300; ++i)
        CHECK(bs.is_set(i) == pattern[i]);

    for (isize i = 299; i >= 0; --i)
        CHECK(bs.pop_back() == pattern[i]);
    CHECK(bs.size() == 0);
    CHECK(bs.none_set()); // popping must leave no bits behind in the words

    bs.push_back(true);
    CHECK(bs.size() == 1);
    CHECK(bs.is_set(0));
    bs.remove_back();
    CHECK(bs.size() == 0);
}

TEST("bitset - reserve and shrink_to_fit never disturb content")
{
    auto const pattern = make_pattern(150, 11);
    auto bs = bitset_of(pattern);
    auto const expected = bs.set_bit_count();

    bs.reserve(4000);
    CHECK(bs.capacity() >= 4000);
    CHECK(bs.size() == 150);
    CHECK(bs.set_bit_count() == expected);

    bs.shrink_to_fit();
    CHECK(bs.capacity() == 192); // 150 bits round up to three u64 words
    CHECK(bs.set_bit_count() == expected);
    for (isize i = 0; i < 150; ++i)
        CHECK(bs.is_set(i) == pattern[i]);

    bs.clear();
    CHECK(bs.size() == 0);
    CHECK(bs.capacity() == 192); // clear keeps the capacity
    bs.shrink_to_fit();
    CHECK(bs.capacity() == 0);
}

TEST("bitset - clear zeroes the words it keeps")
{
    auto bs = cc::bitset::create_filled(100, true);
    bs.clear();
    CHECK(bs.size() == 0);
    bs.resize_to_filled(100);
    CHECK(bs.none_set());
}

TEST("bitset - unset_all keeps the size")
{
    auto bs = cc::bitset::create_filled(100, true);
    bs.unset_all();
    CHECK(bs.size() == 100);
    CHECK(bs.none_set());
}

TEST("bitset - ranges across word boundaries")
{
    auto bs = cc::bitset::create_defaulted(200);

    bs.set_range(60, 10);
    CHECK(bs.set_bit_count() == 10);
    CHECK(bs.find_first_set() == 60);
    CHECK(bs.find_last_set() == 69);

    bs.unset_range(62, 4);
    CHECK(bs.set_bit_count() == 6);

    bs.unset_all();
    bs.toggle_range(0, 200);
    CHECK(bs.all_set());
    bs.toggle_range(0, 200);
    CHECK(bs.none_set());

    bs.set_range(0, 0); // empty range is a no-op
    CHECK(bs.none_set());
}

TEST("bitset - find over word boundaries")
{
    auto bs = cc::bitset::create_defaulted(200);
    bs.set(0);
    bs.set(64);
    bs.set(130);

    CHECK(bs.find_first_set() == 0);
    CHECK(bs.find_first_set(1) == 64);
    CHECK(bs.find_first_set(65) == 130);
    CHECK(bs.find_first_set(131) == -1);
    CHECK(bs.find_first_set(200) == -1);

    CHECK(bs.find_last_set() == 130);
    CHECK(bs.find_last_set(130) == 64);
    CHECK(bs.find_last_set(0) == -1);

    bs.set_all();
    bs.unset(70);
    CHECK(bs.find_first_unset() == 70);
    CHECK(bs.find_last_unset() == 70);
    CHECK(bs.find_first_unset(71) == -1);
}

TEST("bitset - set_indices and unset_indices match a per-index loop")
{
    auto const pattern = make_pattern(300, 99);
    auto const bs = bitset_of(pattern);

    auto expected_set = cc::vector<isize>();
    auto expected_unset = cc::vector<isize>();
    for (isize i = 0; i < 300; ++i)
        (pattern[i] ? expected_set : expected_unset).push_back(i);

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
}

TEST("bitset - whole-set operations")
{
    auto const pa = make_pattern(130, 1);
    auto const pb = make_pattern(130, 2);
    auto const a = bitset_of(pa);
    auto const b = bitset_of(pb);

    auto expected_union = 0;
    auto expected_intersection = 0;
    auto expected_difference = 0;
    auto expected_symmetric = 0;
    for (isize i = 0; i < 130; ++i)
    {
        expected_union += pa[i] || pb[i] ? 1 : 0;
        expected_intersection += pa[i] && pb[i] ? 1 : 0;
        expected_difference += pa[i] && !pb[i] ? 1 : 0;
        expected_symmetric += pa[i] != pb[i] ? 1 : 0;
    }

    CHECK(cc::bitset::create_union_of(a, b).set_bit_count() == expected_union);
    CHECK(cc::bitset::create_intersection_of(a, b).set_bit_count() == expected_intersection);
    CHECK(cc::bitset::create_difference_of(a, b).set_bit_count() == expected_difference);
    CHECK(cc::bitset::create_symmetric_difference_of(a, b).set_bit_count() == expected_symmetric);
    CHECK(a.intersection_bit_count(b) == expected_intersection);

    auto m = a;
    m.set_all_of(b);
    CHECK(m == cc::bitset::create_union_of(a, b));
    m = a;
    m.retain_all_of(b);
    CHECK(m == cc::bitset::create_intersection_of(a, b));
}

TEST("bitset - subset and disjointness")
{
    auto whole = cc::bitset::create_defaulted(70);
    whole.set_range(0, 40);
    auto part = cc::bitset::create_defaulted(70);
    part.set_range(10, 5);
    auto other = cc::bitset::create_defaulted(70);
    other.set_range(50, 5);

    CHECK(whole.has_all(part));
    CHECK(!part.has_all(whole));
    CHECK(whole.has_any(part));
    CHECK(whole.is_disjoint(other));
    CHECK(whole.intersection_bit_count(part) == 5);
}

TEST("bitset - value semantics")
{
    auto a = cc::bitset::create_defaulted(200);
    a.set(3);
    a.set(150);

    auto b = a; // deep copy
    CHECK(b == a);
    b.set(7);
    CHECK(!a.is_set(7));
    CHECK(a != b);

    // Assigning a smaller bitset onto a larger one must not leave the old high words behind.
    auto small = cc::bitset::create_defaulted(10);
    small.set(1);
    b = small;
    CHECK(b.size() == 10);
    CHECK(b.set_bit_count() == 1);
    CHECK(b == small);

    auto moved = cc::move(a);
    CHECK(moved.set_bit_count() == 2);
    CHECK(a.size() == 0); // NOLINT(bugprone-use-after-move) — a moved-from bitset is empty
}

TEST("bitset - equality and hashing")
{
    auto a = cc::bitset::create_defaulted(70);
    auto b = cc::bitset::create_defaulted(70);
    CHECK(a == b);
    CHECK(hash(a) == hash(b));

    a.set(69);
    CHECK(a != b);
    b.set(69);
    CHECK(a == b);
    CHECK(hash(a) == hash(b));

    // Different sizes are never equal, whatever the words hold.
    auto const shorter = cc::bitset::create_defaulted(69);
    CHECK(shorter != cc::bitset::create_defaulted(70));
}

TEST("bitset - operator[] proxy")
{
    auto bs = cc::bitset::create_defaulted(70);
    bs[5] = true;
    CHECK(bs.is_set(5));
    bs[6] = bs[5];
    CHECK(bs[6]);
    bs[5] = false;
    CHECK(bs[6]);

    auto const& cbs = bs;
    CHECK(cbs[6]);
    CHECK(!cbs[5]);
}
