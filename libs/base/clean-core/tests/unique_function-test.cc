#include <clean-core/string.hh>
#include <clean-core/unique_function.hh>
#include <clean-core/vector.hh>

#include <nexus/test.hh>

#include <memory>

// =========================================================================================================
// Helper types for unique_function testing
// =========================================================================================================

namespace
{
// Move-only functor
struct MoveOnlyAdder
{
    std::unique_ptr<int> base;

    explicit MoveOnlyAdder(int val) : base(std::make_unique<int>(val)) {}

    MoveOnlyAdder(MoveOnlyAdder&&) = default;
    MoveOnlyAdder& operator=(MoveOnlyAdder&&) = default;
    MoveOnlyAdder(MoveOnlyAdder const&) = delete;
    MoveOnlyAdder& operator=(MoveOnlyAdder const&) = delete;

    int operator()(int x) const { return *base + x; }
};

// Non-moveable, pinned functor (can only be constructed in-place)
struct PinnedCounter
{
    int count = 0;

    PinnedCounter() = default;
    PinnedCounter(PinnedCounter&&) = delete;
    PinnedCounter(PinnedCounter const&) = delete;
    PinnedCounter& operator=(PinnedCounter&&) = delete;
    PinnedCounter& operator=(PinnedCounter const&) = delete;

    int operator()() { return ++count; }
};

// Stateful mutable functor
struct Counter
{
    int count = 0;
    int operator()() { return ++count; }
};

// Type with member function
struct S
{
    int value = 0;
    int call_count = 0;

    int add(int x)
    {
        ++call_count;
        return value + x;
    }

    int add_const(int x) const { return value + x; }

    int& get_value() { return value; }
};

} // namespace

// =========================================================================================================
// Construction and validity tests
// =========================================================================================================

TEST("unique_function - default construction creates invalid state")
{
    cc::unique_function<int()> f;

    CHECK(!f.is_valid());
    CHECK(!f);
}

TEST("unique_function - construction from stateless lambda")
{
    SECTION("0-ary function")
    {
        auto lambda = []() { return 42; };
        cc::unique_function<int()> f(cc::move(lambda));

        CHECK(f.is_valid());
        CHECK(f);
        CHECK(f() == 42);
    }

    SECTION("unary function")
    {
        auto lambda = [](int x) { return x * 2; };
        cc::unique_function<int(int)> f(cc::move(lambda));

        CHECK(f.is_valid());
        CHECK(f(5) == 10);
    }

    SECTION("multi-arg function")
    {
        auto lambda = [](int a, int b, int c) { return a + b + c; };
        cc::unique_function<int(int, int, int)> f(cc::move(lambda));

        CHECK(f(1, 2, 3) == 6);
    }
}

TEST("unique_function - construction from lambda with captures")
{
    SECTION("capture by value")
    {
        int base = 100;
        auto lambda = [base](int x) { return base + x; };
        cc::unique_function<int(int)> f(cc::move(lambda));

        CHECK(f(5) == 105);
    }

    SECTION("mutable lambda maintains state")
    {
        auto lambda = [count = 0]() mutable { return ++count; };
        cc::unique_function<int()> f(cc::move(lambda));

        CHECK(f() == 1);
        CHECK(f() == 2);
        CHECK(f() == 3);
    }

    SECTION("capture unique_ptr")
    {
        auto ptr = std::make_unique<int>(42);
        auto lambda = [p = cc::move(ptr)]() { return *p; };
        cc::unique_function<int()> f(cc::move(lambda));

        CHECK(f() == 42);
    }
}

TEST("unique_function - construction from move-only functor")
{
    MoveOnlyAdder adder(100);
    cc::unique_function<int(int)> f(cc::move(adder));

    CHECK(f.is_valid());
    CHECK(f(5) == 105);
    CHECK(f(20) == 120);
}

TEST("unique_function - create_with for pinned types")
{
    SECTION("pinned counter constructed in-place")
    {
        auto f = cc::unique_function<int()>::create_from<PinnedCounter>(cc::default_node_allocator());

        CHECK(f.is_valid());
        CHECK(f() == 1);
        CHECK(f() == 2);
        CHECK(f() == 3);
    }

    SECTION("in-place construction with arguments")
    {
        auto f = cc::unique_function<int(int)>::create_from<MoveOnlyAdder>(cc::default_node_allocator(), 50);

        CHECK(f.is_valid());
        CHECK(f(5) == 55);
        CHECK(f(10) == 60);
    }
}

TEST("unique_function - validity queries")
{
    SECTION("default constructed is invalid")
    {
        cc::unique_function<void()> f;
        CHECK(!f.is_valid());
        CHECK(!static_cast<bool>(f));
    }

    SECTION("constructed from callable is valid")
    {
        auto lambda = []() {};
        cc::unique_function<void()> f(cc::move(lambda));

        CHECK(f.is_valid());
        CHECK(static_cast<bool>(f));
    }
}

// =========================================================================================================
// Invocation behavior tests
// =========================================================================================================

TEST("unique_function - invocation with various argument counts")
{
    SECTION("0-ary")
    {
        auto f = cc::unique_function<int()>([]() { return 42; });
        CHECK(f() == 42);
    }

    SECTION("unary")
    {
        auto f = cc::unique_function<int(int)>([](int x) { return x * 2; });
        CHECK(f(5) == 10);
    }

    SECTION("binary")
    {
        auto f = cc::unique_function<int(int, int)>([](int a, int b) { return a + b; });
        CHECK(f(3, 4) == 7);
    }

    SECTION("ternary")
    {
        auto f = cc::unique_function<int(int, int, int)>([](int a, int b, int c) { return a * b + c; });
        CHECK(f(2, 3, 4) == 10);
    }
}

TEST("unique_function - return type preservation")
{
    SECTION("return by value")
    {
        auto f = cc::unique_function<int()>([]() { return 42; });

        static_assert(std::is_same_v<decltype(f()), int>);
        CHECK(f() == 42);
    }

    SECTION("return lvalue reference")
    {
        int x = 5;
        auto lambda = [&]() -> int& { return x; };
        auto f = cc::unique_function<int&()>(cc::move(lambda));

        static_assert(std::is_same_v<decltype(f()), int&>);

        int& result = f();
        CHECK(&result == &x);

        f() = 10;
        CHECK(x == 10);
    }

    SECTION("return rvalue reference")
    {
        auto lambda = []() -> int&&
        {
            static int x = 5;
            return cc::move(x);
        };
        auto f = cc::unique_function<int&&()>(cc::move(lambda));

        static_assert(std::is_same_v<decltype(f()), int&&>);

        SUCCEED(); // just static checks
    }

    SECTION("return const lvalue reference")
    {
        int const x = 42;
        auto lambda = [&]() -> int const& { return x; };
        auto f = cc::unique_function<int const&()>(cc::move(lambda));

        static_assert(std::is_same_v<decltype(f()), int const&>);
        CHECK(f() == 42);
    }
}

TEST("unique_function - void return type")
{
    SECTION("void callable")
    {
        int side_effect = 0;
        auto lambda = [&]() { side_effect = 42; };
        auto f = cc::unique_function<void()>(cc::move(lambda));

        f();
        CHECK(side_effect == 42);
    }

    SECTION("void with arguments")
    {
        int result = 0;
        auto lambda = [&](int a, int b) { result = a + b; };
        auto f = cc::unique_function<void(int, int)>(cc::move(lambda));

        f(3, 4);
        CHECK(result == 7);
    }
}

TEST("unique_function - complex return types")
{
    SECTION("return cc::string by value")
    {
        auto f = cc::unique_function<cc::string()>([]() { return cc::string("hello"); });
        CHECK(f() == "hello");
    }

    SECTION("return std::pair")
    {
        auto f = cc::unique_function<std::pair<int, int>(int)>([](int x) { return std::pair{x, x * 2}; });

        auto result = f(5);
        CHECK(result.first == 5);
        CHECK(result.second == 10);
    }

    SECTION("return reference to complex type")
    {
        cc::string s = "test";
        auto lambda = [&]() -> cc::string& { return s; };
        auto f = cc::unique_function<cc::string&()>(cc::move(lambda));

        cc::string& result = f();
        CHECK(&result == &s);

        f() = "modified";
        CHECK(s == "modified");
    }
}

// =========================================================================================================
// Move semantics tests
// =========================================================================================================

TEST("unique_function - move construction transfers ownership")
{
    SECTION("move from valid function")
    {
        auto lambda = [count = 0]() mutable { return ++count; };
        auto f1 = cc::unique_function<int()>(cc::move(lambda));

        CHECK(f1() == 1);

        auto f2 = cc::move(f1);

        CHECK(f2.is_valid());
        CHECK(f2() == 2);
        CHECK(f2() == 3);
    }

    SECTION("move from function with unique_ptr capture")
    {
        auto ptr = std::make_unique<int>(100);
        auto lambda = [p = cc::move(ptr)](int x) { return *p + x; };
        auto f1 = cc::unique_function<int(int)>(cc::move(lambda));

        CHECK(f1(5) == 105);

        auto f2 = cc::move(f1);

        CHECK(f2.is_valid());
        CHECK(f2(10) == 110);
    }

    SECTION("move from default constructed")
    {
        cc::unique_function<int()> f1;
        auto f2 = cc::move(f1);

        CHECK(!f2.is_valid());
    }
}

TEST("unique_function - move assignment transfers ownership")
{
    SECTION("assign from valid to invalid")
    {
        auto lambda = []() { return 42; };
        auto f1 = cc::unique_function<int()>(cc::move(lambda));
        cc::unique_function<int()> f2;

        CHECK(!f2.is_valid());

        f2 = cc::move(f1);

        CHECK(f2.is_valid());
        CHECK(f2() == 42);
    }

    SECTION("assign from valid to valid")
    {
        auto lambda1 = []() { return 10; };
        auto lambda2 = []() { return 20; };

        auto f1 = cc::unique_function<int()>(cc::move(lambda1));
        auto f2 = cc::unique_function<int()>(cc::move(lambda2));

        CHECK(f1() == 10);
        CHECK(f2() == 20);

        f2 = cc::move(f1);

        CHECK(f2() == 10);
    }

    SECTION("assign preserves state")
    {
        auto lambda = [count = 0]() mutable { return ++count; };
        auto f1 = cc::unique_function<int()>(cc::move(lambda));

        CHECK(f1() == 1);
        CHECK(f1() == 2);

        cc::unique_function<int()> f2;
        f2 = cc::move(f1);

        // State is preserved through move
        CHECK(f2() == 3);
        CHECK(f2() == 4);
    }
}

TEST("unique_function - self move assignment is safe")
{
    auto lambda = [count = 0]() mutable { return ++count; };
    auto f = cc::unique_function<int()>(cc::move(lambda));

    CHECK(f() == 1);

    // Self-assignment
    f = cc::move(f);

    // Should still be in a valid (possibly default) state
    // Implementation-defined behavior, but shouldn't crash
}

// =========================================================================================================
// Ownership and lifetime tests
// =========================================================================================================

TEST("unique_function - owns its callable")
{
    SECTION("callable destroyed with function")
    {
        bool destroyed = false;

        struct DestructorTracker
        {
            bool* flag;
            explicit DestructorTracker(bool* f) : flag(f) {}
            ~DestructorTracker() { *flag = true; }
            int operator()() const { return 42; }
        };

        {
            DestructorTracker tracker(&destroyed);
            auto f = cc::unique_function<int()>(cc::move(tracker));
            CHECK(f() == 42);
            CHECK(!destroyed);
        }

        // Function destroyed, should have destroyed the callable
        CHECK(destroyed);
    }

    SECTION("owned unique_ptr is destroyed")
    {
        bool deleted = false;

        struct Deleter
        {
            bool* flag;
            void operator()(int* p) const
            {
                *flag = true;
                delete p;
            }
        };

        {
            std::unique_ptr<int, Deleter> ptr(new int(42), Deleter{&deleted});
            auto lambda = [p = cc::move(ptr)]() { return *p; };
            auto f = cc::unique_function<int()>(cc::move(lambda));
            CHECK(f() == 42);
            CHECK(!deleted);
        }

        CHECK(deleted);
    }
}

TEST("unique_function - subscope assignment remains valid")
{
    cc::unique_function<int(int)> f;

    {
        auto lambda = [base = 100](int x) { return base + x; };
        f = cc::unique_function<int(int)>(cc::move(lambda));
        CHECK(f(5) == 105);
    }

    // Lambda was moved into f, should still be valid outside the scope
    CHECK(f.is_valid());
    CHECK(f(10) == 110);
    CHECK(f(20) == 120);
}

TEST("unique_function - nested scope with state preservation")
{
    cc::unique_function<int()> outer_f;

    {
        auto lambda = [count = 0]() mutable { return ++count; };
        outer_f = cc::unique_function<int()>(cc::move(lambda));

        CHECK(outer_f() == 1);
        CHECK(outer_f() == 2);

        {
            cc::unique_function<int()> inner_f;
            inner_f = cc::move(outer_f);

            CHECK(inner_f() == 3);
            CHECK(inner_f() == 4);

            outer_f = cc::move(inner_f);
        }
    }

    // State preserved through multiple scope changes and moves
    CHECK(outer_f.is_valid());
    CHECK(outer_f() == 5);
    CHECK(outer_f() == 6);
}

TEST("unique_function - captures unique resources correctly")
{
    SECTION("move unique_ptr into lambda")
    {
        auto ptr = std::make_unique<cc::string>("owned");

        auto lambda = [s = cc::move(ptr)]() -> cc::string const& { return *s; };
        auto f = cc::unique_function<cc::string const&()>(cc::move(lambda));

        CHECK(f() == "owned");
    }

    SECTION("move multiple unique resources")
    {
        auto ptr1 = std::make_unique<int>(10);
        auto ptr2 = std::make_unique<int>(20);

        auto lambda = [p1 = cc::move(ptr1), p2 = cc::move(ptr2)]() { return *p1 + *p2; };
        auto f = cc::unique_function<int()>(cc::move(lambda));

        CHECK(f() == 30);
    }

    SECTION("move-only functor")
    {
        MoveOnlyAdder adder(50);
        auto f = cc::unique_function<int(int)>(cc::move(adder));

        CHECK(f(5) == 55);

        auto f2 = cc::move(f);
        CHECK(f2(10) == 60);
    }
}

// =========================================================================================================
// State preservation tests
// =========================================================================================================

TEST("unique_function - mutable state persists across calls")
{
    SECTION("simple counter")
    {
        auto lambda = [count = 0]() mutable { return ++count; };
        auto f = cc::unique_function<int()>(cc::move(lambda));

        CHECK(f() == 1);
        CHECK(f() == 2);
        CHECK(f() == 3);
        CHECK(f() == 4);
        CHECK(f() == 5);
    }

    SECTION("accumulator")
    {
        auto lambda = [sum = 0](int x) mutable
        {
            sum += x;
            return sum;
        };
        auto f = cc::unique_function<int(int)>(cc::move(lambda));

        CHECK(f(10) == 10);
        CHECK(f(5) == 15);
        CHECK(f(3) == 18);
        CHECK(f(7) == 25);
    }

    SECTION("state preserved through moves")
    {
        auto lambda = [count = 0]() mutable { return ++count; };
        auto f1 = cc::unique_function<int()>(cc::move(lambda));

        CHECK(f1() == 1);
        CHECK(f1() == 2);

        auto f2 = cc::move(f1);
        CHECK(f2() == 3);
        CHECK(f2() == 4);

        auto f3 = cc::move(f2);
        CHECK(f3() == 5);
    }
}

TEST("unique_function - stateful functor")
{
    SECTION("counter functor")
    {
        Counter counter;
        auto f = cc::unique_function<int()>(cc::move(counter));

        CHECK(f() == 1);
        CHECK(f() == 2);
        CHECK(f() == 3);
    }

    SECTION("pinned counter via create_with")
    {
        auto f = cc::unique_function<int()>::create_from<PinnedCounter>(cc::default_node_allocator());

        CHECK(f() == 1);
        CHECK(f() == 2);
        CHECK(f() == 3);
    }
}

// =========================================================================================================
// Const-correctness tests
// =========================================================================================================

TEST("unique_function - const function can call mutable callable")
{
    SECTION("mutable lambda through const function")
    {
        auto lambda = [count = 0]() mutable { return ++count; };
        cc::unique_function<int()> const f(cc::move(lambda));

        // Even though f is const, it can modify the callable's state
        CHECK(f() == 1);
        CHECK(f() == 2);
        CHECK(f() == 3);
    }

    SECTION("mutable state through const function pointer")
    {
        auto lambda = [count = 0]() mutable { return ++count; };
        auto f = cc::unique_function<int()>(cc::move(lambda));

        cc::unique_function<int()> const* ptr = &f;

        CHECK((*ptr)() == 1);
        CHECK((*ptr)() == 2);
    }
}

// =========================================================================================================
// Custom allocator tests
// =========================================================================================================

TEST("unique_function - nullptr allocator works")
{
    SECTION("construct with nullptr allocator")
    {
        auto lambda = [](int x) { return x * 2; };
        auto f = cc::unique_function<int(int)>(cc::move(lambda));

        CHECK(f.is_valid());
        CHECK(f(5) == 10);
    }

    SECTION("create_with with nullptr allocator")
    {
        auto f = cc::unique_function<int()>::create_from<PinnedCounter>(cc::default_node_allocator());

        CHECK(f.is_valid());
        CHECK(f() == 1);
    }
}

// =========================================================================================================
// Edge cases and special scenarios
// =========================================================================================================

TEST("unique_function - passing to functions")
{
    auto execute = [](cc::unique_function<int(int)> f, int x) { return f(x) + 1; };

    auto lambda = [](int x) { return x * 2; };
    auto f = cc::unique_function<int(int)>(cc::move(lambda));

    CHECK(execute(cc::move(f), 5) == 11);
}

TEST("unique_function - returning from functions")
{
    auto create_adder = [](int base) -> cc::unique_function<int(int)> { return [base](int x) { return base + x; }; };

    auto f = create_adder(100);
    CHECK(f.is_valid());
    CHECK(f(5) == 105);
    CHECK(f(20) == 120);
}

TEST("unique_function - storing in containers requires move")
{
    SECTION("vector of unique_functions")
    {
        cc::vector<cc::unique_function<int(int)>> functions;

        functions.push_back([](int x) { return x * 2; });
        functions.push_back([](int x) { return x + 10; });
        functions.push_back([](int x) { return x * x; });

        CHECK(functions[0](5) == 10);
        CHECK(functions[1](5) == 15);
        CHECK(functions[2](5) == 25);
    }
}

TEST("unique_function - large captures")
{
    SECTION("capture large data structure")
    {
        auto large_string = cc::string::create_filled(1000, 'x');
        auto lambda = [s = cc::move(large_string)]() { return s.size(); };
        auto f = cc::unique_function<size_t()>(cc::move(lambda));

        CHECK(f() == 1000);
    }

    SECTION("multiple large captures")
    {
        auto s1 = cc::string::create_filled(500, 'a');
        auto s2 = cc::string::create_filled(500, 'b');
        auto lambda = [s1 = cc::move(s1), s2 = cc::move(s2)]() { return s1.size() + s2.size(); };
        auto f = cc::unique_function<size_t()>(cc::move(lambda));

        CHECK(f() == 1000);
    }
}

TEST("unique_function - generic lambda")
{
    SECTION("generic lambda specialized to int")
    {
        auto generic = [](auto x) { return x * 2; };
        auto f = cc::unique_function<int(int)>(cc::move(generic));

        CHECK(f(5) == 10);
    }

    SECTION("generic lambda specialized to double")
    {
        auto generic = [](auto x) { return x * 2; };
        auto f = cc::unique_function<double(double)>(cc::move(generic));

        CHECK(f(2.5) == 5.0);
    }
}
