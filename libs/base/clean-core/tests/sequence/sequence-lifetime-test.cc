#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/ringbuffer.hh>
#include <clean-core/container/tuple.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/sequence/iterator_ranges.hh>
#include <clean-core/sequence/sequence.hh>
#include <nexus/test.hh>

#include <iterator>

using namespace cc::primitive_defines;

// Where a sequence's references live and how long, and which traits survive an adaptor.
// Each test here pins a case the obvious implementation gets wrong.

namespace
{
template <class T>
bool equals(cc::vector<T> const& a, std::initializer_list<T> b)
{
    if (a.size() != isize(b.size()))
        return false;
    isize i = 0;
    for (auto const& x : b)
        if (!(a[i++] == x))
            return false;
    return true;
}

// a value that marks itself when moved from or destroyed, so a read through a stale reference shows
struct marked
{
    int v;

    explicit marked(int x) : v(x) {}
    marked(marked const& rhs) = default;
    marked(marked&& rhs) noexcept : v(rhs.v) { rhs.v = -7; }
    marked& operator=(marked const& rhs) = default;
    marked& operator=(marked&& rhs) noexcept
    {
        v = rhs.v;
        rhs.v = -7;
        return *this;
    }
    ~marked() { v = -999; }

    friend bool operator==(marked const& a, marked const& b) { return a.v == b.v; }
};

auto const positive = [](marked const& m) { return m.v > 0; };

template <class M>
concept has_const_temporary_keys = requires(M const&& m) { static_cast<M const&&>(m).keys(); };
template <class M>
concept has_const_temporary_values = requires(M const&& m) { static_cast<M const&&>(m).values(); };
template <class M>
concept has_const_lvalue_values = requires(M const& m) { m.values(); };

struct particle
{
    int mass;
};

int const& mass_of(particle const& p)
{
    return p.mass;
}

// 1 .. n, each the iterator's own member, so a reference to it is overwritten by the next step
struct stashing_range
{
    struct iterator
    {
        using iterator_category = std::input_iterator_tag;
        using value_type = int;
        using difference_type = long long;

        int current;

        int const& operator*() const { return current; }
        iterator& operator++()
        {
            ++current;
            return *this;
        }
        void operator++(int) { ++current; }
        bool operator==(iterator const& rhs) const { return current == rhs.current; }
    };

    int n;

    [[nodiscard]] iterator begin() const { return {1}; }
    [[nodiscard]] iterator end() const { return {n + 1}; }
};

struct move_only
{
    int v;

    explicit move_only(int x) : v(x) {}
    move_only(move_only&&) = default;
    move_only& operator=(move_only&&) = default;
    move_only(move_only const&) = delete;
    move_only& operator=(move_only const&) = delete;
    ~move_only() = default;
};
} // namespace

TEST("sequence lifetime - windows over prvalues hold them by value")
{
    // iota has positions and yields prvalues, so each window holds its own copies
    CHECK(equals(cc::iota(5).pairwise().map([](isize a, isize b) { return a + b; }).to_vector(),
                 std::initializer_list<isize>{1, 3, 5, 7}));
    CHECK(equals(cc::iota(7).chunks<3>().map([](isize a, isize, isize c) { return a * c; }).to_vector(),
                 std::initializer_list<isize>{0, 15}));
    CHECK(cc::iota(10).is_sorted());
    CHECK(!cc::iota(10).map([](isize i) { return i == 4 ? 0 : i; }).is_sorted());

    auto const v = cc::vector<int>{3, 1, 4, 1, 5};
    CHECK(equals(
        v.values().map([](int x) { return x * 10; }).pairwise().map([](int a, int b) { return b - a; }).to_vector(),
        {-20, 30, -30, 40}));
}

TEST("sequence lifetime - a window calls a map once per element")
{
    auto const v = cc::vector<int>{1, 2, 3, 4};
    int calls = 0;
    auto const counted = [&](int x)
    {
        ++calls;
        return x;
    };

    v.values().map(counted).pairwise().consume();
    CHECK(calls == 4);

    calls = 0;
    CHECK(v.values().map(counted).is_sorted());
    CHECK(calls == 4);

    // a chunk reaches each element once anyway, so it keeps its positions
    calls = 0;
    static_assert(decltype(v.values().map(counted).chunks<2>())::has_known_size);
    v.values().map(counted).chunks<2>().consume();
    CHECK(calls == 4);
}

TEST("sequence lifetime - the element a runtime non-empty check pulls survives the move")
{
    // repeat's items reference its own value, and the filter has no is_empty(), so the first element is pulled
    CHECK(cc::repeat(marked(42), 2).filter(positive).assert_non_empty().first().v == 42);

    if (auto s = cc::repeat(marked(5), 3).filter(positive).try_as_non_empty(); s.has_value())
        CHECK(cc::move(s.value()).map([](marked const& m) { return m.v; }).sum() == 15);
    else
        CHECK(false);

    // a temporary fixed_vector is owned inline, so its elements move with the sequence
    using marked_vector = cc::fixed_vector<marked, 4>;
    CHECK((marked_vector{marked(1), marked(2)}.values().filter(positive).assert_non_empty().first().v == 1));
    auto const sum = marked_vector{marked(3), marked(4)}
                         .values()
                         .filter(positive)
                         .assert_non_empty()
                         .map([](marked const& m) { return m.v; })
                         .sum();
    CHECK(sum == 7);
}

TEST("sequence lifetime - flatten over owned inner containers")
{
    auto const make = [](isize i) { return cc::fixed_array<int, 2>{int(10 * i), int(10 * i + 1)}; };

    // each inner array dies when the next one opens, so a window or dedup over it copies rather than points
    static_assert(!decltype(cc::iota(3).map(make).flatten())::has_borrowed_elements);
    CHECK(equals(cc::iota(3).map(make).flatten().pairwise().map([](int a, int b) { return a * 100 + b; }).to_vector(),
                 {1, 110, 1011, 1120, 2021}));
    CHECK(equals(
        cc::iota(3).map([](isize i) { return cc::fixed_array<int, 2>{int(i), int(i)}; }).flatten().dedup().to_vector(),
        {0, 1, 2}));
}

TEST("sequence lifetime - borrowed elements need a borrowed input")
{
    auto const v = cc::vector<int>{1, 2, 3};
    auto const pick = [](int const& x) -> int const* { return x > 1 ? &x : nullptr; };

    static_assert(decltype(v.values().filter_map(pick))::has_borrowed_elements);
    static_assert(!decltype(cc::vector<int>{1, 2, 3}.values().filter_map(pick))::has_borrowed_elements);
    CHECK((cc::vector<int>{1, 2, 3}.values().filter_map(pick).sum_from(0) == 5));

    // an initializer list's array dies with the expression, so no pointer may escape
    static_assert(!decltype(cc::values_of({1, 2, 3}))::has_borrowed_elements);
    CHECK(cc::values_of({1, 2, 3}).assert_non_empty().sum() == 6);

    // a const temporary can be neither borrowed nor moved from
    static_assert(!has_const_temporary_keys<cc::map<int, int>>);
    static_assert(!has_const_temporary_values<cc::map<int, int>>);
    static_assert(has_const_lvalue_values<cc::map<int, int>>);
    static_assert(!has_const_temporary_values<cc::ringbuffer<int>>);
    static_assert(has_const_lvalue_values<cc::ringbuffer<int>>);
}

TEST("sequence lifetime - enumerate keeps every trait")
{
    auto const v = cc::vector<int>{4, 5, 6};

    static_assert(decltype(v.values().enumerate())::has_known_size);
    static_assert(decltype(v.values().enumerate())::is_reversible);
    static_assert(decltype(v.values().assert_non_empty().enumerate())::is_non_empty);
    static_assert(decltype(cc::repeat(1).enumerate())::is_infinite);

    CHECK(v.values().assert_non_empty().enumerate().map([](isize i, int x) { return i * x; }).last() == 12);
    CHECK(v.values().enumerate().map([](isize i, int x) { return i + x; }).try_last().value() == 8);
    CHECK(v.values().enumerate().reversed().map([](isize i, int) { return i; }).first_or(-1) == 2);
    CHECK(equals(v.values().enumerate().skip(1).map([](isize i, int x) { return i * 10 + x; }).to_vector(),
                 std::initializer_list<isize>{15, 26}));
    CHECK(equals(cc::repeat(7).enumerate().take(3).map([](isize i, int x) { return i + x; }).to_vector(),
                 std::initializer_list<isize>{7, 8, 9}));

    isize sum = 0;
    for (auto&& [i, x] : v.values().enumerate().reversed())
        sum = sum * 10 + i;
    CHECK(sum == 210);
}

TEST("sequence lifetime - zip trims its longer sides without walking them")
{
    auto const v = cc::vector<int>{1, 2, 3};

    // the iota side reaches 10^12: stepping it back one element at a time would never finish
    CHECK(cc::zip(v, cc::iota(1'000'000'000'000)).reversed().map([](int a, isize b) { return a * 10 + b; }).first_or(-1)
          == 32);
    CHECK(cc::zip(v, cc::iota(1'000'000'000'000)).try_last().has_value());
}

TEST("sequence lifetime - take and skip on a compile-time non-empty sequence keep their positions")
{
    auto const a = cc::fixed_array<int, 4>{1, 2, 3, 4};
    using taken = decltype(a.values().take(2));

    static_assert(decltype(a.values())::is_non_empty);
    static_assert(!taken::is_non_empty);
    static_assert(taken::has_known_size && taken::is_reversible);
    static_assert(cc::impl::seq_has_range<taken::source_t>);
    static_assert(cc::impl::seq_has_range<decltype(a.values().skip(1))::source_t>);

    CHECK(equals(a.values().take(2).to_vector(), {1, 2}));
    CHECK(equals(a.values().skip(3).to_vector(), {4}));
    CHECK(equals(a.values().take_last(2).to_vector(), {3, 4}));
}

TEST("sequence lifetime - infinite and non-empty propagate")
{
    static_assert(decltype(cc::vector<int>().values().chain(cc::repeat(1)))::is_non_empty);
    static_assert(decltype(cc::zip(cc::single(1), cc::repeat(2)))::is_non_empty);
    static_assert(decltype(cc::single(1).map([](int) { return cc::repeat(1); }).flatten())::is_infinite);

    // a step past the end does not overflow the size
    auto const v = cc::vector<int>{1, 2, 3};
    CHECK(v.values().step_by(isize(1) << 62).count() == 1);
}

TEST("sequence lifetime - keys and predicates take an index and spread items")
{
    auto const a = cc::vector<int>{1, 5, 2, 8};
    auto const b = cc::vector<int>{3, 4, 6, 7};

    auto const [less, more] = cc::zip(a, b).partition([](int x, int y) { return x < y; });
    CHECK(less.size() == 2);
    CHECK(more.size() == 2);

    auto const by_half = a.values().count_by([](isize i, int) { return i < 2; });
    CHECK(by_half.get(true) == 2);

    CHECK(cc::zip(a, b).assert_non_empty().min_by([](int x, int y) { return y - x; }).get<0>() == 5);
    CHECK(a.values().assert_non_empty().max_by([](isize i, int x) { return x - int(i) * 10; }) == 1);

    auto const sums = cc::zip(a, b).scan(0, [](int acc, int x, int y) { return acc + x * y; }).to_vector();
    CHECK(equals(sums, {3, 23, 35, 91}));

    CHECK(equals(a.values().dedup_by([](isize i, int) { return i / 2; }).to_vector(), {1, 2}));

    auto const m = cc::zip(a, b).to_map([](int x, int) { return x; }, [](isize i, int, int y) { return y + int(i); });
    CHECK(m.get(8) == 10);

    int keys = 0;
    CHECK(a.values().is_sorted_by(
        [&](int x)
        {
            ++keys;
            return x % 3;
        }));
    CHECK(keys == 4);
    CHECK(cc::zip(a, b).is_sorted_by([](int, int y) { return y; }));
}

TEST("sequence lifetime - a pulled pack of references survives the move")
{
    using marked_vector = cc::fixed_vector<marked, 4>;
    auto const both_positive = [](marked const& a, marked const& b) { return a.v > 0 && b.v > 0; };
    auto const digits = [](marked const& a, marked const& b) { return a.v * 10 + b.v; };

    // each side is a temporary fixed_vector, owned inline, and the zip's pack references both
    auto const first = cc::zip(marked_vector{marked(1), marked(2)}, marked_vector{marked(3), marked(4)})
                           .filter(both_positive)
                           .assert_non_empty()
                           .map(digits)
                           .first();
    CHECK(first == 13);

    if (auto s = cc::zip(marked_vector{marked(1), marked(2)}, marked_vector{marked(3), marked(4)})
                     .filter(both_positive)
                     .try_as_non_empty();
        s.has_value())
        CHECK(cc::move(s.value()).map(digits).sum() == 13 + 24);
    else
        CHECK(false);

    // enumerate's pack, and an indexed window's
    auto const indexed = marked_vector{marked(5), marked(6)}
                             .values()
                             .filter(positive)
                             .enumerate()
                             .assert_non_empty()
                             .map([](isize i, marked const& m) { return m.v + int(i); })
                             .first();
    CHECK(indexed == 5);
    auto const pair = marked_vector{marked(1), marked(2), marked(3)}
                          .values()
                          .pairwise()
                          .filter([](marked const&, marked const&) { return true; })
                          .assert_non_empty()
                          .map(digits)
                          .first();
    CHECK(pair == 12);

    // a buffered window's pack references the window's own buffer
    auto const buffered = cc::repeat(marked(7), 3)
                              .filter(positive)
                              .pairwise()
                              .filter([](marked const&, marked const&) { return true; })
                              .assert_non_empty()
                              .map(digits)
                              .first();
    CHECK(buffered == 77);
}

TEST("sequence lifetime - a window over packs of owned references copies them")
{
    auto const make = [](isize i) { return cc::fixed_array<int, 2>{int(10 * i), int(10 * i + 1)}; };
    auto const v = cc::vector<int>{1, 2, 3, 4, 5, 6};

    auto const enumerated
        = cc::iota(3)
              .map(make)
              .flatten()
              .enumerate()
              .pairwise()
              .map([](auto const& a, auto const& b) { return a.template get<1>() * 100 + b.template get<1>(); })
              .to_vector();
    CHECK(equals(enumerated, {1, 110, 1011, 1120, 2021}));

    auto const zipped
        = cc::zip(cc::iota(3).map(make).flatten(), v)
              .pairwise()
              .map([](auto const& a, auto const& b) { return a.template get<0>() * 100 + b.template get<0>(); })
              .to_vector();
    CHECK(equals(zipped, {1, 110, 1011, 1120, 2021}));
}

TEST("sequence lifetime - an adaptor over a non-empty sequence keeps advance and truncate")
{
    auto const a = cc::fixed_array<int, 6>{1, 2, 3, 4, 5, 6};
    int calls = 0;
    auto const counted = [&](int x)
    {
        ++calls;
        return x * 10;
    };

    CHECK(equals(a.values().map(counted).skip(4).to_vector(), {50, 60}));
    CHECK(calls == 2);

    calls = 0;
    CHECK(a.values().map(counted).take(2).try_last().value() == 20);
    CHECK(calls == 1);

    static_assert(decltype(a.values().map(counted))::is_non_empty);
    static_assert(!decltype(a.values().map(counted).take(2))::is_non_empty);
    static_assert(!decltype(a.values().enumerate().reversed().skip(1))::is_non_empty);
    CHECK(a.values().enumerate().reversed().skip(1).map([](isize i, int) { return i; }).first_or(-1) == 4);
}

TEST("sequence lifetime - zip trims its sides with truncate, evaluating nothing")
{
    auto const a = cc::vector<int>{1, 2, 3};
    auto const b = cc::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int calls = 0;
    auto const counted = [&](int x)
    {
        ++calls;
        return x;
    };

    CHECK(cc::zip(a, b.values().map(counted).step_by(1)).try_last().has_value());
    CHECK(calls == 1);

    calls = 0;
    CHECK(cc::zip(a, b.values().map(counted).chunks<1>()).try_last().has_value());
    CHECK(calls == 1);

    calls = 0;
    CHECK(cc::zip(a, b.values().chain(b).map(counted)).try_last().has_value());
    CHECK(calls == 1);

    CHECK(
        cc::zip(a, cc::iota(1'000'000'000'000).step_by(2)).reversed().map([](int x, isize y) { return x + y; }).first_or(-1)
        == 7);
}

TEST("sequence lifetime - a reference a callback returns from its own state is not borrowed")
{
    auto v = cc::vector<int>{0, 1};
    auto const table = cc::fixed_array<int, 2>{5, 6};

    static_assert(decltype(v.values().map([](int& x) -> int& { return x; }))::has_borrowed_elements);
    static_assert(!decltype(v.values().map([table](int i) -> int const& { return table[i]; }))::has_borrowed_elements);

    auto const ps = cc::vector<particle>{{1}, {2}};
    static_assert(decltype(ps.values().map(&particle::mass))::has_borrowed_elements);
    CHECK(ps.values().map(&particle::mass).find([](int m) { return m == 2; }) == &ps[1].mass);
}

TEST("sequence lifetime - a key may take the materialized tuple")
{
    auto const a = cc::vector<int>{1, 5, 2};
    auto const b = cc::vector<int>{3, 4, 6};

    auto const smallest
        = cc::zip(a, b).assert_non_empty().min_by([](cc::tuple<int, int> const& t) { return t.get<1>(); });
    CHECK(smallest.get<0>() == 1);
    auto const groups = cc::zip(a, b).to_grouped_map([](isize i, cc::tuple<int, int> const&) { return i % 2; });
    CHECK(groups.get(0).size() == 2);
}

TEST("sequence lifetime - a buffered window knows its size")
{
    auto const v = cc::vector<int>{1, 2, 3, 4};
    int calls = 0;
    auto const counted = [&](int x)
    {
        ++calls;
        return x;
    };

    static_assert(decltype(v.values().map(counted).pairwise())::has_known_size);
    CHECK(v.values().map(counted).pairwise().count() == 3);
    CHECK(v.values().map(counted).windows<5>().count() == 0);
    CHECK(v.values().map(counted).chunks<3>().count() == 1);
    CHECK(calls == 0);

    auto w = v.values().map(counted).pairwise();
    (void)w.begin(); // range-for's first step: the first window is pulled
    CHECK(cc::move(w).count() == 2);
}

TEST("sequence lifetime - a nested pack materializes as nested tuples of values")
{
    auto a = cc::vector<int>{1, 2, 3};
    auto const b = cc::vector<int>{4, 5, 6};

    auto const pairs = cc::zip(a, b).pairwise().to_vector();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(pairs[0].get<0>())>, cc::tuple<int, int>>);
    auto const indexed = cc::zip(a, b).enumerate().to_vector();
    a[0] = 100;
    CHECK(pairs[0].get<0>().get<0>() == 1);
    CHECK(indexed[0].get<1>().get<0>() == 1);

    // over owned temporaries the materialized copy is all that is left
    using marked_vector = cc::fixed_vector<marked, 4>;
    auto const owned
        = cc::zip(marked_vector{marked(1), marked(2)}, marked_vector{marked(3), marked(4)}).pairwise().to_vector();
    CHECK(owned[0].get<1>().get<1>().v == 4);
    auto const first = cc::zip(marked_vector{marked(5)}, marked_vector{marked(6)}).enumerate().assert_non_empty().first();
    CHECK(first.get<1>().get<0>().v == 5);
}

TEST("sequence lifetime - a negative skip skips nothing")
{
    auto const v = cc::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

    // scan has no advance, so this is the skip_source path, then truncate through it
    CHECK(v.values().scan(0, [](int, int x) { return x; }).skip(-2).count() == 10);
    CHECK(cc::repeat(7, 4).filter([](int) { return true; }).skip(-2).to_vector().size() == 4);
    CHECK(cc::repeat(7, 10).map([](int x) { return x; }).take(4).skip(-2).take(3).count() == 3);

    auto const chunks = v.values().chunks<2>().skip(-3).take(1).to_vector();
    CHECK(chunks.size() == 1);
    CHECK(v.values().chunks<2>().skip(-3).take(1).count() == 1);
    CHECK(v.values().step_by(3).skip(-1).take(2).count() == 2);
}

TEST("sequence lifetime - a buffer keeps references only where they are borrowed")
{
    auto const v = cc::vector<int>{1, 2, 3, 4};

    // a capturing callback may hand out a reference into scratch it overwrites, so a window copies
    auto scratch = [buf = 0](int x) mutable -> int const&
    {
        buf = x * 10;
        return buf;
    };
    auto const pairs = v.values().map(scratch).pairwise().map([](int a, int b) { return a + b; }).to_vector();
    CHECK(equals(pairs, {30, 50, 70}));
    CHECK(v.values().map(scratch).dedup().count() == 4);

    // a stateless projection is borrowed: the window references the elements and writes through them
    auto ps = cc::vector<particle>{{1}, {2}, {3}};
    ps.values().map(&particle::mass).filter([](int) { return true; }).pairwise().each([](int& a, int& b) { b += a; });
    CHECK(ps[2].mass == 6);

    // and so a move-only element can be windowed there
    auto ms = cc::vector<move_only>();
    ms.push_back(move_only(1));
    ms.push_back(move_only(2));
    auto const sum = ms.values()
                         .filter([](move_only const&) { return true; })
                         .pairwise()
                         .map([](move_only const& a, move_only const& b) { return a.v + b.v; })
                         .sum_from(0);
    CHECK(sum == 3);
}

TEST("sequence lifetime - a buffer over the source's own storage survives the source's move")
{
    using marked_array = cc::fixed_array<marked, 3>;
    auto const digits = [](marked const& a, marked const& b) { return a.v * 10 + b.v; };

    // the window keeps copies, so moving the sequence moves nothing it points into
    if (auto s = marked_array{marked(1), marked(2), marked(3)}.values().filter(positive).pairwise().try_as_non_empty();
        s.has_value())
        CHECK(cc::move(s.value()).map(digits).sum() == 12 + 23);
    else
        CHECK(false);
    if (auto s = cc::repeat(marked(7), 3).filter(positive).pairwise().try_as_non_empty(); s.has_value())
        CHECK(cc::move(s.value()).map(digits).sum() == 77 + 77);
    else
        CHECK(false);

    auto const deduped = cc::fixed_array<marked, 4>{marked(1), marked(1), marked(2), marked(2)}
                             .values()
                             .filter(positive)
                             .dedup()
                             .assert_non_empty()
                             .map([](marked const& m) { return m.v; })
                             .to_vector();
    CHECK(equals(deduped, {1, 2}));
}

TEST("sequence lifetime - dedup over an owning source compares before it moves")
{
    auto const deduped = cc::vector<marked>{marked(1), marked(1), marked(2)}.values().dedup().to_vector();
    CHECK(deduped.size() == 2);
    CHECK(deduped[0].v == 1);
    CHECK(deduped[1].v == 2);
}

TEST("sequence lifetime - an owning sequence moves its elements out of every terminal that keeps one")
{
    auto const make = []
    {
        auto ms = cc::vector<move_only>();
        ms.push_back(move_only(3));
        ms.push_back(move_only(1));
        ms.push_back(move_only(2));
        return ms;
    };

    CHECK(make().values().assert_non_empty().first().v == 3);
    CHECK(make().values().try_first().value().v == 3);
    CHECK(make().values().first_or(move_only(-1)).v == 3);
    CHECK(make().values().try_nth(2).value().v == 2);
    auto const [small, large] = make().values().partition([](move_only const& m) { return m.v < 2; });
    CHECK(small.size() == 1);
    CHECK(large.size() == 2);
}

TEST("sequence lifetime - a sized source answers non-emptiness without evaluating")
{
    auto const v = cc::vector<int>{1, 2, 3};
    int calls = 0;
    auto const counted = [&](int x)
    {
        ++calls;
        return x;
    };

    auto s = cc::zip(v.values().map(counted), v).assert_non_empty();
    static_assert(decltype(s)::has_known_size);
    CHECK(calls == 0);
    CHECK(cc::move(s).count() == 3);
    CHECK(v.values().map(counted).step_by(2).try_as_non_empty().has_value());
    CHECK(calls == 0);
}

TEST("sequence lifetime - an input iterator's references are not borrowed")
{
    static_assert(!decltype(cc::values_of(stashing_range{3}))::has_borrowed_elements);
    auto const pairs
        = cc::values_of(stashing_range{4}).pairwise().map([](int a, int b) { return a * 10 + b; }).to_vector();
    CHECK(equals(pairs, {12, 23, 34}));
}

TEST("sequence lifetime - a function pointer returns borrowed references")
{
    auto const ps = cc::vector<particle>{{1}, {2}};
    static_assert(decltype(ps.values().map(&mass_of))::has_borrowed_elements);
    CHECK(ps.values().map(&mass_of).find([](int m) { return m == 2; }) == &ps[1].mass);
}

TEST("sequence lifetime - a key may take the tuple by rvalue")
{
    auto const a = cc::vector<int>{3, 1, 2};
    auto const b = cc::vector<int>{4, 5, 6};
    auto const smallest = cc::zip(a, b).assert_non_empty().min_by([](cc::tuple<int, int>&& t) { return t.get<0>(); });
    CHECK(smallest.get<1>() == 5);
}
