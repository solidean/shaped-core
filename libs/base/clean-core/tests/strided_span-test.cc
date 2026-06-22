#include <clean-core/strided_span.hh>
#include <clean-core/utility.hh>
#include <clean-core/vector.hh>

#include <nexus/test.hh>

// static assertions for triviality
static_assert(std::is_trivially_copyable_v<cc::strided_span<int>>, "strided_span should be trivially copyable");
static_assert(std::is_trivially_copyable_v<cc::strided_iterator<int>>, "strided_iterator should be trivially copyable");

// verify triviality even with non-trivial element type
namespace
{
struct non_trivial // NOLINT
{
    int value = 0;
    ~non_trivial() {} // makes it non-trivial
};
} // namespace

static_assert(std::is_trivially_copyable_v<cc::strided_span<non_trivial>>,
              "strided_span should be trivially copyable even with non-trivial T");

TEST("strided_span - construction")
{
    SECTION("default construction")
    {
        auto const s = cc::strided_span<int>{};
        CHECK(s.start_ptr() == nullptr);
        CHECK(s.size() == 0);
        CHECK(s.stride_bytes() == 0);
        CHECK(s.empty());
        CHECK(s.is_contiguous());
    }

    SECTION("pointer + size + stride construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        CHECK(s.start_ptr() == data);
        CHECK(s.size() == 5);
        CHECK(s.stride_bytes() == sizeof(int));
        CHECK(!s.empty());
    }

    SECTION("initializer_list construction")
    {
        auto values = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int const>(values);
        CHECK(s.size() == 5);
        CHECK(s.stride_bytes() == sizeof(int));
        CHECK(s[0] == 1);
        CHECK(s[4] == 5);
    }

    SECTION("container construction - vector")
    {
        auto vec = cc::vector<int>{1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{vec};
        CHECK(s.start_ptr() == vec.data());
        CHECK(s.size() == 5);
        CHECK(s.stride_bytes() == sizeof(int));
        CHECK(s[0] == 1);
        CHECK(s[4] == 5);
    }

    SECTION("container construction - const vector")
    {
        auto const vec = cc::vector<int>{1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int const>{vec};
        CHECK(s.start_ptr() == vec.data());
        CHECK(s.size() == 5);
        CHECK(s.stride_bytes() == sizeof(int));
    }

    SECTION("C array construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data};
        CHECK(s.start_ptr() == data);
        CHECK(s.size() == 5);
        CHECK(s.stride_bytes() == sizeof(int));
        CHECK(s[0] == 1);
        CHECK(s[4] == 5);
    }

    SECTION("const C array construction")
    {
        int const data[] = {10, 20, 30};
        auto const s = cc::strided_span<int const>{data};
        CHECK(s.start_ptr() == data);
        CHECK(s.size() == 3);
        CHECK(s.stride_bytes() == sizeof(int));
        CHECK(s[0] == 10);
        CHECK(s[2] == 30);
    }
}

TEST("strided_span - factory methods")
{
    SECTION("create_from_single")
    {
        int value = 42;
        auto const s = cc::strided_span<int>::create_from_single(value);
        CHECK(s.start_ptr() == &value);
        CHECK(s.size() == 1);
        CHECK(s.stride_bytes() == sizeof(int));
        CHECK(s[0] == 42);
        CHECK(s.front() == 42);
        CHECK(s.back() == 42);
    }

    SECTION("create_from_repeated")
    {
        int value = 99;
        auto const s = cc::strided_span<int>::create_from_repeated(value, 5);
        CHECK(s.start_ptr() == &value);
        CHECK(s.size() == 5);
        CHECK(s.stride_bytes() == 0);

        // All indices should access the same memory location
        CHECK(s[0] == 99);
        CHECK(s[1] == 99);
        CHECK(s[2] == 99);
        CHECK(s[3] == 99);
        CHECK(s[4] == 99);

        // Mutating through any index changes all
        s[2] = 77;
        CHECK(value == 77);
        CHECK(s[0] == 77);
        CHECK(s[4] == 77);
    }

    SECTION("create_from_repeated with count 0")
    {
        int value = 42;
        auto const s = cc::strided_span<int>::create_from_repeated(value, 0);
        CHECK(s.size() == 0);
        CHECK(s.empty());
    }
}

TEST("strided_span - element access")
{
    SECTION("operator[] with contiguous data")
    {
        int data[] = {10, 20, 30, 40, 50};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        CHECK(s[0] == 10);
        CHECK(s[1] == 20);
        CHECK(s[2] == 30);
        CHECK(s[3] == 40);
        CHECK(s[4] == 50);
    }

    SECTION("operator[] - mutation")
    {
        int data[] = {10, 20, 30, 40, 50};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        s[2] = 99;
        CHECK(data[2] == 99);
        CHECK(s[2] == 99);
    }

    SECTION("operator[] with custom stride")
    {
        // Simulate interleaved data: every other int
        int data[] = {1, 0, 2, 0, 3, 0, 4, 0, 5, 0};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(2 * sizeof(int))};
        CHECK(s[0] == 1);
        CHECK(s[1] == 2);
        CHECK(s[2] == 3);
        CHECK(s[3] == 4);
        CHECK(s[4] == 5);
    }

    SECTION("front")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::strided_span<int>{data, 3, static_cast<cc::isize>(sizeof(int))};
        CHECK(s.front() == 10);
    }

    SECTION("back")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::strided_span<int>{data, 3, static_cast<cc::isize>(sizeof(int))};
        CHECK(s.back() == 30);
    }

    SECTION("back with custom stride")
    {
        int data[] = {1, 0, 2, 0, 3, 0};
        auto const s = cc::strided_span<int>{data, 3, static_cast<cc::isize>(2 * sizeof(int))};
        CHECK(s.back() == 3);
    }

    SECTION("start_ptr")
    {
        int data[] = {10, 20, 30};
        auto const s = cc::strided_span<int>{data, 3, static_cast<cc::isize>(sizeof(int))};
        CHECK(s.start_ptr() == data);
    }
}

TEST("strided_span - interleaved data scenario")
{
    SECTION("struct-of-arrays pattern")
    {
        struct Vertex
        {
            float x, y, z;
            int color;
        };

        Vertex vertices[] = {
            {1.0f, 2.0f, 3.0f, 10},
            {4.0f, 5.0f, 6.0f, 20},
            {7.0f, 8.0f, 9.0f, 30},
        };

        // View just the x coordinates
        auto x_coords = cc::strided_span<float>{&vertices[0].x, 3, static_cast<cc::isize>(sizeof(Vertex))};
        CHECK(x_coords[0] == 1.0f);
        CHECK(x_coords[1] == 4.0f);
        CHECK(x_coords[2] == 7.0f);

        // View just the colors
        auto colors = cc::strided_span<int>{&vertices[0].color, 3, static_cast<cc::isize>(sizeof(Vertex))};
        CHECK(colors[0] == 10);
        CHECK(colors[1] == 20);
        CHECK(colors[2] == 30);

        // Mutate through strided view
        x_coords[1] = 99.0f;
        CHECK(vertices[1].x == 99.0f);
    }
}

TEST("strided_span - iterators")
{
    SECTION("begin/end with contiguous data")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        auto it = s.begin();
        CHECK(*it == 1);
        ++it;
        CHECK(*it == 2);
    }

    SECTION("range-based for loop")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};

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
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};

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

    SECTION("range-based for loop with custom stride")
    {
        int data[] = {1, 0, 2, 0, 3, 0, 4, 0, 5, 0};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(2 * sizeof(int))};

        int sum = 0;
        for (auto const& val : s)
        {
            sum += val;
        }
        CHECK(sum == 15);
    }

    SECTION("range-based for loop with repeated element (stride 0)")
    {
        int value = 7;
        auto const s = cc::strided_span<int>::create_from_repeated(value, 5);

        int count = 0;
        for (auto const& val : s)
        {
            CHECK(val == 7);
            ++count;
        }
        CHECK(count == 5);
    }
}

TEST("strided_span - queries")
{
    SECTION("size and empty")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        CHECK(s.size() == 5);
        CHECK(!s.empty());

        auto const empty_s = cc::strided_span<int>{};
        CHECK(empty_s.size() == 0);
        CHECK(empty_s.empty());
    }

    SECTION("stride_bytes")
    {
        int data[] = {1, 2, 3};
        auto const s1 = cc::strided_span<int>{data, 3, static_cast<cc::isize>(sizeof(int))};
        CHECK(s1.stride_bytes() == sizeof(int));

        auto const s2 = cc::strided_span<int>{data, 3, static_cast<cc::isize>(2 * sizeof(int))};
        CHECK(s2.stride_bytes() == 2 * sizeof(int));

        auto const s3 = cc::strided_span<int>::create_from_repeated(data[0], 5);
        CHECK(s3.stride_bytes() == 0);
    }

    SECTION("is_contiguous - size 0 or 1")
    {
        int data = 42;
        auto const s0 = cc::strided_span<int>{};
        CHECK(s0.is_contiguous());

        auto const s1 = cc::strided_span<int>::create_from_single(data);
        CHECK(s1.is_contiguous());

        // Even with non-standard stride, size 1 is contiguous
        auto const s1_weird = cc::strided_span<int>{&data, 1, 999};
        CHECK(s1_weird.is_contiguous());
    }

    SECTION("is_contiguous - stride == sizeof(T)")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        CHECK(s.is_contiguous());
    }

    SECTION("is_contiguous - stride != sizeof(T)")
    {
        int data[] = {1, 0, 2, 0, 3, 0};
        auto const s = cc::strided_span<int>{data, 3, static_cast<cc::isize>(2 * sizeof(int))};
        CHECK(!s.is_contiguous());
    }

    SECTION("is_contiguous - stride 0")
    {
        int value = 42;
        auto const s = cc::strided_span<int>::create_from_repeated(value, 5);
        CHECK(!s.is_contiguous());
    }
}

TEST("strided_span - operations")
{
    SECTION("reversed with contiguous data")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        auto const rev = s.reversed();

        CHECK(rev.size() == 5);
        CHECK(rev.stride_bytes() == -static_cast<cc::isize>(sizeof(int)));
        CHECK(rev[0] == 5);
        CHECK(rev[1] == 4);
        CHECK(rev[2] == 3);
        CHECK(rev[3] == 2);
        CHECK(rev[4] == 1);
    }

    SECTION("reversed iteration")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        auto const rev = s.reversed();

        cc::vector<int> collected;
        for (auto const& val : rev)
        {
            collected.push_back(val);
        }

        CHECK(collected.size() == 5);
        CHECK(collected[0] == 5);
        CHECK(collected[1] == 4);
        CHECK(collected[2] == 3);
        CHECK(collected[3] == 2);
        CHECK(collected[4] == 1);
    }

    SECTION("reversed with custom stride")
    {
        int data[] = {1, 0, 2, 0, 3, 0};
        auto const s = cc::strided_span<int>{data, 3, static_cast<cc::isize>(2 * sizeof(int))};
        auto const rev = s.reversed();

        CHECK(rev.size() == 3);
        CHECK(rev.stride_bytes() == -static_cast<cc::isize>(2 * sizeof(int)));
        CHECK(rev[0] == 3);
        CHECK(rev[1] == 2);
        CHECK(rev[2] == 1);
    }

    SECTION("reversed empty span")
    {
        auto const s = cc::strided_span<int>{};
        auto const rev = s.reversed();
        CHECK(rev.empty());
    }

    SECTION("double reverse")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        auto const rev = s.reversed().reversed();

        CHECK(rev.size() == 5);
        CHECK(rev.stride_bytes() == static_cast<cc::isize>(sizeof(int)));
        CHECK(rev[0] == 1);
        CHECK(rev[1] == 2);
        CHECK(rev[2] == 3);
        CHECK(rev[3] == 4);
        CHECK(rev[4] == 5);
    }
}

TEST("strided_span - try_to_span conversion")
{
    SECTION("contiguous conversion succeeds")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{data, 5, static_cast<cc::isize>(sizeof(int))};
        auto const maybe_span = s.try_to_span();

        CHECK(maybe_span.has_value());
        auto const span = maybe_span.value();
        CHECK(span.data() == data);
        CHECK(span.size() == 5);
        CHECK(span[0] == 1);
        CHECK(span[4] == 5);
    }

    SECTION("non-contiguous conversion fails")
    {
        int data[] = {1, 0, 2, 0, 3, 0};
        auto const s = cc::strided_span<int>{data, 3, static_cast<cc::isize>(2 * sizeof(int))};
        auto const maybe_span = s.try_to_span();

        CHECK(!maybe_span.has_value());
    }

    SECTION("empty span conversion succeeds")
    {
        auto const s = cc::strided_span<int>{};
        auto const maybe_span = s.try_to_span();

        CHECK(maybe_span.has_value());
        auto const span = maybe_span.value();
        CHECK(span.size() == 0);
    }

    SECTION("size 1 span conversion succeeds")
    {
        int value = 42;
        auto const s = cc::strided_span<int>::create_from_single(value);
        auto const maybe_span = s.try_to_span();

        CHECK(maybe_span.has_value());
        auto const span = maybe_span.value();
        CHECK(span.size() == 1);
        CHECK(span[0] == 42);
    }

    SECTION("repeated span conversion fails")
    {
        int value = 42;
        auto const s = cc::strided_span<int>::create_from_repeated(value, 5);
        auto const maybe_span = s.try_to_span();

        CHECK(!maybe_span.has_value());
    }
}

TEST("strided_span - const correctness")
{
    SECTION("strided_span<T const> from const data")
    {
        int const data[] = {1, 2, 3};
        auto const s = cc::strided_span<int const>{data, 3, static_cast<cc::isize>(sizeof(int))};
        CHECK(s[0] == 1);
        // s[0] = 99; // should not compile
    }

    SECTION("strided_span<T const> from mutable data")
    {
        int data[] = {1, 2, 3};
        auto const s = cc::strided_span<int const>{data, 3, static_cast<cc::isize>(sizeof(int))};
        CHECK(s[0] == 1);
        // s[0] = 99; // should not compile
        data[0] = 99; // but we can still mutate through original pointer
        CHECK(s[0] == 99);
    }

    SECTION("const strided_span<T> still allows mutation")
    {
        int data[] = {1, 2, 3};
        auto const s = cc::strided_span<int>{data, 3, static_cast<cc::isize>(sizeof(int))};
        s[0] = 99; // const span, but T is mutable
        CHECK(data[0] == 99);
    }
}

TEST("strided_span - copy and move")
{
    SECTION("copy construction")
    {
        int data[] = {1, 2, 3};
        auto const s1 = cc::strided_span<int>{data, 3, static_cast<cc::isize>(sizeof(int))};
        auto const s2 = s1;
        CHECK(s2.start_ptr() == data);
        CHECK(s2.size() == 3);
        CHECK(s2.stride_bytes() == sizeof(int));
        CHECK(s1.start_ptr() == data);
        CHECK(s1.size() == 3);
    }

    SECTION("copy assignment")
    {
        int data1[] = {1, 2, 3};
        int data2[] = {4, 5, 6, 7};
        auto s1 = cc::strided_span<int>{data1, 3, static_cast<cc::isize>(sizeof(int))};
        auto s2 = cc::strided_span<int>{data2, 4, static_cast<cc::isize>(sizeof(int))};
        s2 = s1;
        CHECK(s2.start_ptr() == data1);
        CHECK(s2.size() == 3);
    }

    SECTION("move construction")
    {
        int data[] = {1, 2, 3};
        auto s1 = cc::strided_span<int>{data, 3, static_cast<cc::isize>(sizeof(int))};
        auto const s2 = cc::move(s1);
        CHECK(s2.start_ptr() == data);
        CHECK(s2.size() == 3);
    }

    SECTION("move assignment")
    {
        int data1[] = {1, 2, 3};
        int data2[] = {4, 5, 6, 7};
        auto s1 = cc::strided_span<int>{data1, 3, static_cast<cc::isize>(sizeof(int))};
        auto s2 = cc::strided_span<int>{data2, 4, static_cast<cc::isize>(sizeof(int))};
        s2 = cc::move(s1);
        CHECK(s2.start_ptr() == data1);
        CHECK(s2.size() == 3);
    }
}

TEST("strided_span - negative stride")
{
    SECTION("manual negative stride construction")
    {
        int data[] = {1, 2, 3, 4, 5};
        // Start at end, stride backwards
        auto const s = cc::strided_span<int>{&data[4], 5, -static_cast<cc::isize>(sizeof(int))};
        CHECK(s[0] == 5);
        CHECK(s[1] == 4);
        CHECK(s[2] == 3);
        CHECK(s[3] == 2);
        CHECK(s[4] == 1);
    }

    SECTION("negative stride iteration")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{&data[4], 5, -static_cast<cc::isize>(sizeof(int))};

        cc::vector<int> collected;
        for (auto const& val : s)
        {
            collected.push_back(val);
        }

        CHECK(collected.size() == 5);
        CHECK(collected[0] == 5);
        CHECK(collected[1] == 4);
        CHECK(collected[2] == 3);
        CHECK(collected[3] == 2);
        CHECK(collected[4] == 1);
    }

    SECTION("negative stride is not contiguous")
    {
        int data[] = {1, 2, 3, 4, 5};
        auto const s = cc::strided_span<int>{&data[4], 5, -static_cast<cc::isize>(sizeof(int))};
        CHECK(!s.is_contiguous());
    }
}
