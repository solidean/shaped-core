#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

// static assertions for triviality
static_assert(std::is_trivially_copyable_v<cc::span<int>>, "span should be trivially copyable");
static_assert(std::is_trivially_copyable_v<cc::fixed_span<int, 5>>, "fixed_span should be trivially copyable");

// verify implicit conversion from span<T> to span<T const>
static_assert(std::is_convertible_v<cc::span<int>, cc::span<int const>>,
              "span<int> should be implicitly convertible to span<int const>");

// verify triviality even with non-trivial element type
namespace
{
struct non_trivial // NOLINT
{
    int value = 0;
    ~non_trivial() {} // makes it non-trivial
};
} // namespace

static_assert(std::is_trivially_copyable_v<cc::span<non_trivial>>,
              "span should be trivially copyable even with non-trivial T");
static_assert(std::is_trivially_copyable_v<cc::fixed_span<non_trivial, 3>>,
              "fixed_span should be trivially copyable even with non-trivial T");

TEST("span - construction")
{
    SECTION("default construction")
    {
        auto const s = cc::span<int>{};
        CHECK(s.data() == nullptr);
        CHECK(s.size() == 0);
        CHECK(s.empty());
    }

    SECTION("pointer + size construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::span<int>{data, 5};
        CHECK(s.data() == data);
        CHECK(s.size() == 5);
        CHECK(!s.empty());
    }

    SECTION("two pointer construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::span<int>{data, data + 5};
        CHECK(s.data() == data);
        CHECK(s.size() == 5);
    }

    SECTION("initializer_list construction")
    {
        auto values = {1, 2, 3, 4, 5};
        auto const s = cc::span<int const>(values);
        CHECK(s.size() == 5);
        CHECK(s[0] == 1);
        CHECK(s[4] == 5);
    }

    SECTION("container construction - vector")
    {
        auto vec = cc::vector<int>{1, 2, 3, 4, 5};
        auto const s = cc::span<int>{vec};
        CHECK(s.data() == vec.data());
        CHECK(s.size() == 5);
        CHECK(s[0] == 1);
        CHECK(s[4] == 5);
    }

    SECTION("container construction - const vector")
    {
        auto const vec = cc::vector<int>{1, 2, 3, 4, 5};
        auto const s = cc::span<int const>{vec};
        CHECK(s.data() == vec.data());
        CHECK(s.size() == 5);
    }

    SECTION("C array construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::span<int>{data};
        CHECK(s.data() == data);
        CHECK(s.size() == 5);
        CHECK(s[0] == 1);
        CHECK(s[4] == 5);
    }

    SECTION("const C array construction")
    {
        int const data[] = {10, 20, 30};
        auto const s = cc::span<int const>{data};
        CHECK(s.data() == data);
        CHECK(s.size() == 3);
        CHECK(s[0] == 10);
        CHECK(s[2] == 30);
    }
}

TEST("span - element access")
{
    SECTION("operator[]")
    {
        int data[] = {10, 20, 30, 40, 50};
        auto const s = cc::span<int>{data, 5};
        CHECK(s[0] == 10);
        CHECK(s[1] == 20);
        CHECK(s[2] == 30);
        CHECK(s[3] == 40);
        CHECK(s[4] == 50);
    }

    SECTION("operator[] - mutation")
    {
        int data[] = {10, 20, 30, 40, 50};
        auto const s = cc::span<int>{data, 5};
        s[2] = 99;
        CHECK(data[2] == 99);
        CHECK(s[2] == 99);
    }

    SECTION("front")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::span<int>{data, 3};
        CHECK(s.front() == 10);
    }

    SECTION("back")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::span<int>{data, 3};
        CHECK(s.back() == 30);
    }

    SECTION("data")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::span<int>{data, 3};
        CHECK(s.data() == data);
    }
}

TEST("span - iterators")
{
    SECTION("begin/end")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::span<int>{data, 5};
        CHECK(s.begin() == data);
        CHECK(s.end() == data + 5);
    }

    SECTION("range-based for loop")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::span<int>{data, 5};

        int sum = 0;
        for (auto const& val : s)
        {
            sum += val;
        }
        CHECK(sum == 15);
    }

    SECTION("range-based for loop - mutation")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::span<int>{data, 5};

        for (auto& val : s)
        {
            val *= 2;
        }

        CHECK(data[0] == 2);
        CHECK(data[1] == 4);
        CHECK(data[2] == 6);
        CHECK(data[3] == 8);
        CHECK(data[4] == 10);
    }
}

TEST("span - const correctness")
{
    SECTION("span<T const> from const data")
    {
        int const data[] = {1, 2, 3};
        auto const s = cc::span<int const>{data, 3};
        CHECK(s[0] == 1);
        // s[0] = 99; // should not compile
    }

    SECTION("span<T const> from mutable data")
    {
        int data[] = {1, 2, 3};
        auto const s = cc::span<int const>{data, 3};
        CHECK(s[0] == 1);
        // s[0] = 99; // should not compile
        data[0] = 99; // but we can still mutate through original pointer
        CHECK(s[0] == 99);
    }

    SECTION("const span<T> still allows mutation")
    {
        int data[] = {1, 2, 3};
        auto const s = cc::span<int>{data, 3};
        s[0] = 99; // const span, but T is mutable
        CHECK(data[0] == 99);
    }
}

TEST("span - copy and move")
{
    SECTION("copy construction")
    {
        int data[] = {1, 2, 3};
        auto const s1 = cc::span<int>{data, 3};
        auto const s2 = s1;
        CHECK(s2.data() == data);
        CHECK(s2.size() == 3);
        CHECK(s1.data() == data);
        CHECK(s1.size() == 3);
    }

    SECTION("copy assignment")
    {
        int data1[] = {1, 2, 3};
        int data2[] = {4, 5, 6, 7};
        auto s1 = cc::span<int>{data1, 3};
        auto s2 = cc::span<int>{data2, 4};
        s2 = s1;
        CHECK(s2.data() == data1);
        CHECK(s2.size() == 3);
    }

    SECTION("move construction")
    {
        int data[] = {1, 2, 3};
        auto s1 = cc::span<int>{data, 3};
        auto const s2 = cc::move(s1);
        CHECK(s2.data() == data);
        CHECK(s2.size() == 3);
    }

    SECTION("move assignment")
    {
        int data1[] = {1, 2, 3};
        int data2[] = {4, 5, 6, 7};
        auto s1 = cc::span<int>{data1, 3};
        auto s2 = cc::span<int>{data2, 4};
        s2 = cc::move(s1);
        CHECK(s2.data() == data1);
        CHECK(s2.size() == 3);
    }
}

TEST("span - function arguments")
{
    auto sum_span = [](cc::span<int const> s) -> int
    {
        int sum = 0;
        for (auto val : s)
        {
            sum += val;
        }
        return sum;
    };

    SECTION("pass vector by temp construction")
    {
        auto const result = sum_span(cc::span<int const>{cc::vector<int>{1, 2, 3, 4, 5}});
        CHECK(result == 15);
    }

    SECTION("pass vector by explicit construction")
    {
        auto vec = cc::vector<int>{1, 2, 3, 4, 5};
        auto const result = sum_span(cc::span<int const>{vec});
        CHECK(result == 15);
    }

    SECTION("pass array")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const result = sum_span(cc::span<int const>{data, 5});
        CHECK(result == 15);
    }

    SECTION("pass C array directly")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const result = sum_span(data);
        CHECK(result == 15);
    }
}

TEST("fixed_span - construction")
{
    SECTION("default construction")
    {
        auto const s = cc::fixed_span<int, 0>{};
        CHECK(s.data() == nullptr);
        CHECK(s.size() == 0);
        CHECK(s.empty());
    }

    SECTION("pointer construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::fixed_span<int, 5>(data);
        CHECK(s.data() == data);
        CHECK(s.size() == 5);
        CHECK(!s.empty());
    }

    SECTION("pointer + size construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::fixed_span<int, 5>{data, 5};
        CHECK(s.data() == data);
        CHECK(s.size() == 5);
    }

    SECTION("two pointer construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::fixed_span<int, 5>{data, data + 5};
        CHECK(s.data() == data);
        CHECK(s.size() == 5);
    }

    SECTION("initializer_list construction")
    {
        auto values = {1, 2, 3, 4, 5};
        auto const s = cc::fixed_span<int const, 5>{values};
        CHECK(s.size() == 5);
        CHECK(s[0] == 1);
        CHECK(s[4] == 5);
    }

    SECTION("container construction - vector")
    {
        auto vec = cc::vector<int>{1, 2, 3, 4, 5};
        auto const s = cc::fixed_span<int, 5>{vec};
        CHECK(s.data() == vec.data());
        CHECK(s.size() == 5);
    }

    SECTION("C array construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::fixed_span<int, 5>{data};
        CHECK(s.data() == data);
        CHECK(s.size() == 5);
        CHECK(s[0] == 1);
        CHECK(s[4] == 5);
    }

    SECTION("const C array construction")
    {
        int const data[] = {10, 20, 30};
        auto const s = cc::fixed_span<int const, 3>{data};
        CHECK(s.data() == data);
        CHECK(s.size() == 3);
        CHECK(s[0] == 10);
        CHECK(s[2] == 30);
    }
}

TEST("fixed_span - element access")
{
    SECTION("operator[]")
    {
        int data[] = {10, 20, 30, 40, 50};
        auto const s = cc::fixed_span<int, 5>{data};
        CHECK(s[0] == 10);
        CHECK(s[1] == 20);
        CHECK(s[2] == 30);
        CHECK(s[3] == 40);
        CHECK(s[4] == 50);
    }

    SECTION("operator[] - mutation")
    {
        int data[] = {10, 20, 30, 40, 50};
        auto const s = cc::fixed_span<int, 5>{data};
        s[2] = 99;
        CHECK(data[2] == 99);
        CHECK(s[2] == 99);
    }

    SECTION("front")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::fixed_span<int, 3>{data};
        CHECK(s.front() == 10);
    }

    SECTION("back")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::fixed_span<int, 3>{data};
        CHECK(s.back() == 30);
    }

    SECTION("data")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::fixed_span<int, 3>{data};
        CHECK(s.data() == data);
    }
}

TEST("fixed_span - iterators")
{
    SECTION("begin/end")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::fixed_span<int, 5>{data};
        CHECK(s.begin() == data);
        CHECK(s.end() == data + 5);
    }

    SECTION("range-based for loop")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::fixed_span<int, 5>{data};

        int sum = 0;
        for (auto const& val : s)
        {
            sum += val;
        }
        CHECK(sum == 15);
    }

    SECTION("range-based for loop - mutation")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::fixed_span<int, 5>{data};

        for (auto& val : s)
        {
            val *= 2;
        }

        CHECK(data[0] == 2);
        CHECK(data[1] == 4);
        CHECK(data[2] == 6);
        CHECK(data[3] == 8);
        CHECK(data[4] == 10);
    }
}

TEST("fixed_span - const correctness")
{
    SECTION("fixed_span<T const> from const data")
    {
        int const data[] = {1, 2, 3};
        auto const s = cc::fixed_span<int const, 3>{data};
        CHECK(s[0] == 1);
        // s[0] = 99; // should not compile
    }

    SECTION("const fixed_span<T> still allows mutation")
    {
        int data[] = {1, 2, 3};
        auto const s = cc::fixed_span<int, 3>{data};
        s[0] = 99; // const span, but T is mutable
        CHECK(data[0] == 99);
    }
}

TEST("fixed_span - copy and move")
{
    SECTION("copy construction")
    {
        int data[] = {1, 2, 3};
        auto const s1 = cc::fixed_span<int, 3>{data};
        auto const s2 = s1;
        CHECK(s2.data() == data);
        CHECK(s2.size() == 3);
        CHECK(s1.data() == data);
    }

    SECTION("copy assignment")
    {
        int data1[] = {1, 2, 3};
        int data2[] = {4, 5, 6};
        auto s1 = cc::fixed_span<int, 3>{data1};
        auto s2 = cc::fixed_span<int, 3>{data2};
        s2 = s1;
        CHECK(s2.data() == data1);
        CHECK(s2.size() == 3);
    }

    SECTION("move construction")
    {
        int data[] = {1, 2, 3};
        auto s1 = cc::fixed_span<int, 3>{data};
        auto const s2 = cc::move(s1);
        CHECK(s2.data() == data);
        CHECK(s2.size() == 3);
    }

    SECTION("move assignment")
    {
        int data1[] = {1, 2, 3};
        int data2[] = {4, 5, 6};
        auto s1 = cc::fixed_span<int, 3>{data1};
        auto s2 = cc::fixed_span<int, 3>{data2};
        s2 = cc::move(s1);
        CHECK(s2.data() == data1);
        CHECK(s2.size() == 3);
    }
}

TEST("fixed_span - tuple protocol")
{
    SECTION("get member function")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::fixed_span<int, 3>{data};
        CHECK(s.get<0>() == 10);
        CHECK(s.get<1>() == 20);
        CHECK(s.get<2>() == 30);
    }

    SECTION("structured bindings")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::fixed_span<int, 3>{data};
        auto const [a, b, c] = s;
        CHECK(a == 10);
        CHECK(b == 20);
        CHECK(c == 30);
    }

    SECTION("structured bindings - mutation")
    {
        int data[] = {10, 20, 30};
        auto s = cc::fixed_span<int, 3>{data};
        auto& [a, b, c] = s;
        a = 99;
        b = 88;
        c = 77;
        CHECK(data[0] == 99);
        CHECK(data[1] == 88);
        CHECK(data[2] == 77);
    }

    SECTION("tuple_size")
    {
        using span_type = cc::fixed_span<int, 5>;
        CHECK(std::tuple_size_v<span_type> == 5);
    }

    SECTION("tuple_element")
    {
        using span_type = cc::fixed_span<int, 3>;
        static_assert(std::is_same_v<std::tuple_element_t<0, span_type>, int>);
        static_assert(std::is_same_v<std::tuple_element_t<1, span_type>, int>);
        static_assert(std::is_same_v<std::tuple_element_t<2, span_type>, int>);

        SUCCEED(); // just static checks
    }
}

TEST("fixed_span - empty span")
{
    SECTION("N = 0 is valid")
    {
        auto const s = cc::fixed_span<int, 0>{};
        CHECK(s.size() == 0);
        CHECK(s.empty());
    }

    SECTION("empty span iteration")
    {
        auto const s = cc::fixed_span<int, 0>{};
        int count = 0;
        for ([[maybe_unused]] auto const& val : s)
        {
            ++count;
        }
        CHECK(count == 0);
    }
}

TEST("span - braced init list construction")
{
    SECTION("span from braced init")
    {
        auto check_span = [](cc::span<int const> s)
        {
            CHECK(s.size() == 3);
            CHECK(s[0] == 1);
            CHECK(s[1] == 2);
            CHECK(s[2] == 3);
        };
        check_span({1, 2, 3});
    }
}

TEST("fixed_span - braced init list construction")
{
    SECTION("fixed_span from braced init")
    {
        auto check_span = [](cc::fixed_span<int const, 3> s)
        {
            CHECK(s.size() == 3);
            CHECK(s[0] == 1);
            CHECK(s[1] == 2);
            CHECK(s[2] == 3);
        };
        check_span(cc::fixed_span<int const, 3>{1, 2, 3});
    }
}

TEST("span - subspan")
{
    int data[] = {0, 1, 2, 3, 4, 5};
    auto const s = cc::span<int>{data, 6};

    SECTION("subspan(offset)")
    {
        auto const sub = s.subspan(2);
        CHECK(sub.size() == 4);
        CHECK(sub[0] == 2);
        CHECK(sub[3] == 5);

        CHECK(s.subspan(0).size() == 6);
        CHECK(s.subspan(6).empty());
    }

    SECTION("subspan(offset_size)")
    {
        auto const sub = s.subspan({.offset = 1, .size = 3});
        CHECK(sub.size() == 3);
        CHECK(sub[0] == 1);
        CHECK(sub[2] == 3);

        CHECK(s.subspan({.offset = 6, .size = 0}).empty());
        CHECK(s.subspan({.offset = 0, .size = 6}).size() == 6);
    }

    SECTION("subspan(start_end)")
    {
        auto const sub = s.subspan({.start = 2, .end = 5});
        CHECK(sub.size() == 3);
        CHECK(sub[0] == 2);
        CHECK(sub[2] == 4);

        CHECK(s.subspan({.start = 3, .end = 3}).empty());
        CHECK(s.subspan({.start = 0, .end = 6}).size() == 6);
    }
}

TEST("span - is_subspan")
{
    int data[] = {0, 1, 2, 3};
    auto const s = cc::span<int>{data, 4};

    SECTION("offset")
    {
        CHECK(s.is_subspan(0));
        CHECK(s.is_subspan(4));
        CHECK(!s.is_subspan(5));
        CHECK(!s.is_subspan(-1));
    }

    SECTION("offset_size")
    {
        CHECK(s.is_subspan(cc::offset_size{.offset = 1, .size = 3}));
        CHECK(s.is_subspan(cc::offset_size{.offset = 4, .size = 0}));
        CHECK(!s.is_subspan(cc::offset_size{.offset = 2, .size = 3}));
        CHECK(!s.is_subspan(cc::offset_size{.offset = -1, .size = 1}));
    }

    SECTION("start_end")
    {
        CHECK(s.is_subspan(cc::start_end{.start = 1, .end = 3}));
        CHECK(s.is_subspan(cc::start_end{.start = 0, .end = 4}));
        CHECK(!s.is_subspan(cc::start_end{.start = 2, .end = 5}));
        CHECK(!s.is_subspan(cc::start_end{.start = -1, .end = 2}));
    }
}

TEST("span - subspan_clamped")
{
    int data[] = {0, 1, 2, 3, 4};
    auto const s = cc::span<int>{data, 5};

    SECTION("offset")
    {
        CHECK(s.subspan_clamped(2).size() == 3);
        CHECK(s.subspan_clamped(-3).size() == 5); // clamped to 0
        CHECK(s.subspan_clamped(10).empty());     // clamped to size
    }

    SECTION("offset_size")
    {
        CHECK(s.subspan_clamped({.offset = 1, .size = 2}).size() == 2);
        CHECK(s.subspan_clamped({.offset = 3, .size = 10}).size() == 2); // size truncated
        CHECK(s.subspan_clamped({.offset = 10, .size = 3}).empty());     // offset clamped
        CHECK(s.subspan_clamped({.offset = -2, .size = 3}).size() == 3); // offset clamped to 0
    }

    SECTION("start_end")
    {
        CHECK(s.subspan_clamped({.start = 1, .end = 3}).size() == 2);
        CHECK(s.subspan_clamped({.start = 2, .end = 99}).size() == 3); // end clamped
        CHECK(s.subspan_clamped({.start = -5, .end = 2}).size() == 2); // start clamped to 0
        CHECK(s.subspan_clamped({.start = 99, .end = 99}).empty());    // both clamped to size
    }
}

// constexpr smoke test
static_assert(
    []
    {
        int data[] = {0, 1, 2, 3};
        auto const s = cc::span<int>{data, 4};
        return s.subspan(1).size() == 3                        //
            && s.subspan({.offset = 1, .size = 2}).size() == 2 //
            && s.subspan({.start = 1, .end = 3}).size() == 2   //
            && s.is_subspan(2)                                 //
            && s.subspan_clamped(10).empty();
    }(),
    "span subspan should be usable in constexpr");

// span is a borrow range
static_assert(cc::enable_borrowed_range<cc::span<int>>, "span<T> should be a borrow range");
static_assert(cc::enable_borrowed_range<cc::fixed_span<int, 3>>, "fixed_span should be a borrow range");
static_assert(!cc::enable_borrowed_range<cc::vector<int>>, "vector is not a borrow range");

TEST("span - size_bytes")
{
    int data[] = {1, 2, 3, 4};
    auto const s = cc::span<int>{data, 4};
    CHECK(s.size_bytes() == 4 * cc::isize(sizeof(int)));
    CHECK(cc::span<int>{}.size_bytes() == 0);
}

TEST("span - first_n / last_n")
{
    int data[] = {0, 1, 2, 3, 4};
    auto const s = cc::span<int>{data, 5};

    SECTION("first_n")
    {
        CHECK(s.first_n(0).empty());
        CHECK(s.first_n(2).size() == 2);
        CHECK(s.first_n(2)[0] == 0);
        CHECK(s.first_n(2)[1] == 1);
        CHECK(s.first_n(5).size() == 5);
    }

    SECTION("last_n")
    {
        CHECK(s.last_n(0).empty());
        CHECK(s.last_n(2).size() == 2);
        CHECK(s.last_n(2)[0] == 3);
        CHECK(s.last_n(2)[1] == 4);
        CHECK(s.last_n(5).size() == 5);
    }

    SECTION("clamped")
    {
        CHECK(s.first_n_clamped(-3).empty());
        CHECK(s.first_n_clamped(99).size() == 5);
        CHECK(s.last_n_clamped(-3).empty());
        CHECK(s.last_n_clamped(99).size() == 5);
        CHECK(s.last_n_clamped(2)[0] == 3);
    }
}

TEST("span - reinterpret_as / bytes")
{
    SECTION("reinterpret to smaller")
    {
        int data[] = {1, 2};
        auto const s = cc::span<int>{data, 2};
        auto const shorts = s.reinterpret_as<short>();
        CHECK(shorts.size() == 4);
        CHECK(s.as_bytes().size() == 8);
    }

    SECTION("as_mutable_bytes writes through")
    {
        int x = 0;
        auto const s = cc::span<int>{&x, 1};
        auto const bytes = s.as_mutable_bytes();
        CHECK(bytes.size() == cc::isize(sizeof(int)));
        for (auto& b : bytes)
            b = cc::byte{0xFF};
        CHECK(x == -1); // all bits set
    }

    SECTION("as_bytes on const span")
    {
        int const data[] = {1, 2, 3};
        auto const s = cc::span<int const>{data, 3};
        auto const bytes = s.as_bytes();
        static_assert(std::is_same_v<decltype(bytes), cc::span<cc::byte const> const>);
        CHECK(bytes.size() == 12);
    }
}

TEST("span - try_reinterpret_as")
{
    cc::byte buf[8] = {};
    auto const s = cc::span<cc::byte>{buf, 8};

    SECTION("divisible succeeds")
    {
        auto const r = s.try_reinterpret_as<int>();
        REQUIRE(r.has_value());
        CHECK(r.value().size() == 2);
    }

    SECTION("indivisible yields nullopt")
    {
        auto const r = s.first_n(5).try_reinterpret_as<int>();
        CHECK(!r.has_value());
    }
}

TEST("span - free as_bytes / as_mutable_bytes")
{
    SECTION("container yields byte view")
    {
        cc::vector<int> v = {1, 2, 3};
        auto const bytes = cc::as_bytes(v);
        static_assert(std::is_same_v<decltype(bytes), cc::span<cc::byte const> const>);
        CHECK(bytes.size() == 3 * cc::isize(sizeof(int)));
    }

    SECTION("const container yields span<byte const>")
    {
        cc::vector<int> const v = {1, 2};
        auto const bytes = cc::as_bytes(v);
        static_assert(std::is_same_v<decltype(bytes), cc::span<cc::byte const> const>);
        CHECK(bytes.size() == 2 * cc::isize(sizeof(int)));
    }

    SECTION("string_view yields its chars as bytes")
    {
        cc::string_view const sv = "abc";
        auto const bytes = cc::as_bytes(sv);
        CHECK(bytes.size() == 3);
        CHECK(bytes[0] == cc::byte('a'));
    }

    SECTION("as_mutable_bytes writes through")
    {
        cc::vector<int> v = {0};
        auto const bytes = cc::as_mutable_bytes(v);
        static_assert(std::is_same_v<decltype(bytes), cc::span<cc::byte> const>);
        for (auto& b : bytes)
            b = cc::byte{0xFF};
        CHECK(v[0] == -1); // all bits set
    }
}
