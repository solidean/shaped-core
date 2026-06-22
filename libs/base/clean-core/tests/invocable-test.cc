#include <clean-core/utility.hh>

#include <nexus/test.hh>

#include <memory>
#include <string>

// =========================================================================================================
// Helper types for invoke testing
// =========================================================================================================

namespace
{
// Type with member functions and data
struct S
{
    int value = 0;
    int call_count = 0;

    int f(int x)
    {
        ++call_count;
        return value + x;
    }

    int f_const(int x) const { return value + x; }
};

// Type with member object pointer
struct MO
{
    int x;
};

// Custom smart-pointer-like type (proxy dereference)
struct P
{
    S* p;
    S& operator*() const { return *p; }
};

// Callable with ref-qualified operator()
struct Q
{
    int operator()() & { return 1; }
    int operator()() && { return 2; }
};

// Callable that distinguishes lvalue/rvalue args
struct G
{
    int operator()(int&) { return 1; }
    int operator()(int&&) { return 2; }
};

// Type that is both callable and has members (for overload-set sanity test)
struct CallableWithMembers
{
    int call_counter = 0;
    int operator_call_counter = 0;

    int method()
    {
        ++call_counter;
        return 100;
    }

    int operator()()
    {
        ++operator_call_counter;
        return 200;
    }
};

} // namespace

// =========================================================================================================
// 0-ary callable tests
// =========================================================================================================

TEST("invoke - 0-ary callable calls and returns value")
{
    SECTION("lambda returning value")
    {
        auto f = []() { return 42; };
        CHECK(cc::invoke(f) == 42);
    }

    SECTION("struct with operator()")
    {
        struct F
        {
            int operator()() const { return 99; }
        };
        F func;
        CHECK(cc::invoke(func) == 99);
    }
}

TEST("invoke - 0-ary callable returns lvalue reference (decltype(auto) correctness)")
{
    SECTION("lambda returning lvalue reference")
    {
        int x = 5;
        auto f = [&]() -> int& { return x; };

        static_assert(std::is_same_v<decltype(cc::invoke(f)), int&>);

        int& ref = cc::invoke(f);
        CHECK(&ref == &x);

        // Modify through returned reference
        cc::invoke(f) = 10;
        CHECK(x == 10);
    }

    SECTION("struct returning reference")
    {
        int data = 42;
        struct RefReturner
        {
            int& ref;
            int& operator()() { return ref; }
        };
        RefReturner r{data};

        static_assert(std::is_same_v<decltype(cc::invoke(r)), int&>);
        cc::invoke(r) = 99;
        CHECK(data == 99);
    }
}

TEST("invoke - 0-ary callable forwarding of callable itself (operator() ref-qualifiers)")
{
    SECTION("lvalue vs rvalue operator()")
    {
        Q q;
        CHECK(cc::invoke(q) == 1);           // lvalue -> operator()&
        CHECK(cc::invoke(cc::move(q)) == 2); // rvalue -> operator()&&
        CHECK(cc::invoke(Q{}) == 2);         // prvalue -> operator()&&
    }
}

// =========================================================================================================
// N-ary normal call tests
// =========================================================================================================

TEST("invoke - N-ary normal call forwards args (lvalue/rvalue overload set)")
{
    SECTION("arg forwarding preserves value category")
    {
        G g;
        int x = 10;

        CHECK(cc::invoke(g, x) == 1);      // lvalue arg -> lvalue overload
        CHECK(cc::invoke(g, 0) == 2);      // rvalue arg -> rvalue overload
        CHECK(cc::invoke(g, int{5}) == 2); // prvalue -> rvalue overload
    }

    SECTION("multiple args")
    {
        auto f = [](int a, int b, int c) { return a + b + c; };
        CHECK(cc::invoke(f, 1, 2, 3) == 6);
    }
}

// =========================================================================================================
// Member function pointer tests
// =========================================================================================================

TEST("invoke - member function pointer on object lvalue")
{
    SECTION("basic member function call")
    {
        S s;
        s.value = 10;

        int result = cc::invoke(&S::f, s, 7);
        CHECK(result == 17);
        CHECK(s.call_count == 1);
    }

    SECTION("matches direct call")
    {
        S s;
        s.value = 5;

        int direct = (s.*&S::f)(3);
        s.call_count = 0;
        s.value = 5;
        int via_invoke = cc::invoke(&S::f, s, 3);

        CHECK(via_invoke == direct);
    }
}

TEST("invoke - member function pointer on object pointer")
{
    SECTION("raw pointer")
    {
        S s;
        s.value = 20;
        S* p = &s;

        int result = cc::invoke(&S::f, p, 5);
        CHECK(result == 25);
        CHECK(s.call_count == 1);
    }

    SECTION("matches deref syntax")
    {
        S s;
        s.value = 15;
        S* p = &s;

        int direct = ((*p).*&S::f)(10);
        s.call_count = 0;
        s.value = 15;
        int via_invoke = cc::invoke(&S::f, p, 10);

        CHECK(via_invoke == direct);
    }
}

TEST("invoke - member function pointer on smart pointer")
{
    SECTION("unique_ptr")
    {
        static_assert(cc::is_invocable<decltype(&S::f), std::unique_ptr<S>&, int>);
        static_assert(cc::is_invocable_r<int, decltype(&S::f), std::unique_ptr<S>&, int>);

        auto up = std::make_unique<S>();
        up->value = 30;

        int result = cc::invoke(&S::f, up, 12);
        CHECK(result == 42);
        CHECK(up->call_count == 1);
    }

    SECTION("shared_ptr")
    {
        static_assert(cc::is_invocable<decltype(&S::f), std::shared_ptr<S>&, int>);
        static_assert(cc::is_invocable_r<int, decltype(&S::f), std::shared_ptr<S>&, int>);

        auto sp = std::make_shared<S>();
        sp->value = 100;

        int result = cc::invoke(&S::f, sp, 23);
        CHECK(result == 123);
        CHECK(sp->call_count == 1);
    }
}

// =========================================================================================================
// Member object pointer tests
// =========================================================================================================

TEST("invoke - member object pointer on object lvalue returns reference")
{
    SECTION("decltype returns reference")
    {
        MO m{5};
        static_assert(std::is_same_v<decltype(cc::invoke(&MO::x, m)), int&>);

        SUCCEED(); // just static checks
    }

    SECTION("can read and write through reference")
    {
        MO m{5};
        CHECK(cc::invoke(&MO::x, m) == 5);

        cc::invoke(&MO::x, m) = 9;
        CHECK(m.x == 9);
    }

    SECTION("reference identity")
    {
        MO m{42};
        int& ref = cc::invoke(&MO::x, m);
        CHECK(&ref == &m.x);
    }
}

TEST("invoke - member object pointer on pointer and smart pointer")
{
    SECTION("raw pointer")
    {
        MO m{10};
        MO* p = &m;

        static_assert(std::is_same_v<decltype(cc::invoke(&MO::x, p)), int&>);
        CHECK(cc::invoke(&MO::x, p) == 10);

        cc::invoke(&MO::x, p) = 20;
        CHECK(m.x == 20);
    }

    SECTION("unique_ptr")
    {
        auto up = std::make_unique<MO>();
        up->x = 30;

        static_assert(std::is_same_v<decltype(cc::invoke(&MO::x, up)), int&>);
        CHECK(cc::invoke(&MO::x, up) == 30);

        cc::invoke(&MO::x, up) = 40;
        CHECK(up->x == 40);
    }

    SECTION("shared_ptr")
    {
        auto sp = std::make_shared<MO>();
        sp->x = 50;

        CHECK(cc::invoke(&MO::x, sp) == 50);
        cc::invoke(&MO::x, sp) = 60;
        CHECK(sp->x == 60);
    }
}

// =========================================================================================================
// is_invocable tests
// =========================================================================================================

TEST("invoke - is_invocable agrees with invoke for positive cases")
{
    SECTION("0-ary callable")
    {
        auto f = []() { return 42; };
        using F = decltype(f);
        static_assert(cc::is_invocable<F>);

        SUCCEED(); // just static checks
    }

    SECTION("normal callables with args")
    {
        auto f = [](int, float) { return 1.0; };
        using F = decltype(f);
        static_assert(cc::is_invocable<F, int, float>);
        static_assert(cc::is_invocable<F, int&, float&>);
        static_assert(cc::is_invocable<F, int&&, float&&>);

        SUCCEED(); // just static checks
    }

    SECTION("member function pointers")
    {
        static_assert(cc::is_invocable<decltype(&S::f), S&, int>);
        static_assert(cc::is_invocable<decltype(&S::f), S*, int>);
        static_assert(cc::is_invocable<decltype(&S::f), std::unique_ptr<S>&, int>);

        SUCCEED(); // just static checks
    }

    SECTION("member object pointers")
    {
        static_assert(cc::is_invocable<decltype(&MO::x), MO&>);
        static_assert(cc::is_invocable<decltype(&MO::x), MO*>);
        static_assert(cc::is_invocable<decltype(&MO::x), std::unique_ptr<MO>&>);

        SUCCEED(); // just static checks
    }
}

TEST("invoke - is_invocable rejects wrong-arity/wrong-types for normal callables")
{
    SECTION("wrong arity")
    {
        struct H
        {
            void operator()(int) {}
        };

        static_assert(!cc::is_invocable<H>);           // missing arg
        static_assert(cc::is_invocable<H, int>);       // correct
        static_assert(!cc::is_invocable<H, int, int>); // too many args

        SUCCEED(); // just static checks
    }

    SECTION("type mismatch without implicit conversion")
    {
        struct OnlyInt
        {
            void operator()(int*) {}
        };

        static_assert(cc::is_invocable<OnlyInt, int*>);
        static_assert(!cc::is_invocable<OnlyInt, int>); // int doesn't convert to int*

        SUCCEED(); // just static checks
    }
}

TEST("invoke - member object pointer rejects extra args via is_invocable")
{
    SECTION("member object pointer with extra args is not invocable")
    {
        static_assert(!cc::is_invocable<decltype(&MO::x), MO&, int>);
        static_assert(!cc::is_invocable<decltype(&MO::x), MO*, int>);
        static_assert(!cc::is_invocable<decltype(&MO::x), std::unique_ptr<MO>&, int>);

        SUCCEED(); // just static checks
    }

    SECTION("correct usage has no extra args")
    {
        static_assert(cc::is_invocable<decltype(&MO::x), MO&>);
        static_assert(cc::is_invocable<decltype(&MO::x), MO*>);

        SUCCEED(); // just static checks
    }
}

TEST("invoke - is_invocable documents implicit conversions")
{
    SECTION("implicit numeric conversions allowed")
    {
        struct I
        {
            int operator()(int) { return 0; }
        };

        static_assert(cc::is_invocable<I, int>);
        static_assert(cc::is_invocable<I, short>); // short -> int
        static_assert(cc::is_invocable<I, char>);  // char -> int
        static_assert(cc::is_invocable<I, bool>);  // bool -> int

        SUCCEED(); // just static checks
    }

    SECTION("implicit pointer conversions")
    {
        struct PtrFunc
        {
            void operator()(void const*) {}
        };

        static_assert(cc::is_invocable<PtrFunc, int*>);           // int* -> void const*
        static_assert(cc::is_invocable<PtrFunc, char*>);          // char* -> void const*
        static_assert(cc::is_invocable<PtrFunc, std::nullptr_t>); // nullptr -> void const*

        SUCCEED(); // just static checks
    }
}

// =========================================================================================================
// is_invocable_r tests
// =========================================================================================================

TEST("invoke - is_invocable_r for convertible vs non-convertible return types")
{
    SECTION("convertible return types")
    {
        auto f_int = []() { return 42; };
        using F = decltype(f_int);

        static_assert(cc::is_invocable_r<int, F>);
        static_assert(cc::is_invocable_r<long, F>);   // int -> long
        static_assert(cc::is_invocable_r<float, F>);  // int -> float
        static_assert(cc::is_invocable_r<double, F>); // int -> double

        SUCCEED(); // just static checks
    }

    SECTION("non-convertible return types")
    {
        auto f_string = []() { return std::string("hello"); };
        using F = decltype(f_string);

        static_assert(cc::is_invocable_r<std::string, F>);
        static_assert(!cc::is_invocable_r<int, F>);         // string doesn't convert to int
        static_assert(!cc::is_invocable_r<char const*, F>); // string doesn't implicitly convert to char*

        SUCCEED(); // just static checks
    }

    SECTION("reference return types")
    {
        int x = 5;
        auto f_ref = [&]() -> int& { return x; };
        using F = decltype(f_ref);

        static_assert(cc::is_invocable_r<int&, F>);
        static_assert(cc::is_invocable_r<int, F>);        // int& -> int (copy)
        static_assert(cc::is_invocable_r<int const&, F>); // int& -> int const&

        SUCCEED(); // just static checks
    }
}

TEST("invoke - is_invocable_r void accepts any return type")
{
    SECTION("void target accepts int return")
    {
        struct RV
        {
            int operator()() { return 1; }
        };
        static_assert(cc::is_invocable_r<void, RV>);

        SUCCEED(); // just static checks
    }

    SECTION("void target accepts void return")
    {
        struct RVo
        {
            void operator()() {}
        };
        static_assert(cc::is_invocable_r<void, RVo>);

        SUCCEED(); // just static checks
    }

    SECTION("void target accepts string return")
    {
        auto f = []() { return std::string("test"); };
        using F = decltype(f);
        static_assert(cc::is_invocable_r<void, F>);

        SUCCEED(); // just static checks
    }

    SECTION("void target accepts reference return")
    {
        int x = 5;
        auto f = [&]() -> int& { return x; };
        using F = decltype(f);
        static_assert(cc::is_invocable_r<void, F>);

        SUCCEED(); // just static checks
    }
}

// =========================================================================================================
// Overload-set sanity tests
// =========================================================================================================

TEST("invoke - member-pointer path wins when F is a member pointer")
{
    SECTION("type is both callable and has member - member pointer selects method")
    {
        CallableWithMembers obj;

        // Direct call uses operator()
        obj();
        CHECK(obj.operator_call_counter == 1);
        CHECK(obj.call_counter == 0);

        // Member pointer explicitly selects the method
        cc::invoke(&CallableWithMembers::method, obj);
        CHECK(obj.call_counter == 1);
        CHECK(obj.operator_call_counter == 1);
    }

    SECTION("verify method is actually called via member pointer")
    {
        CallableWithMembers obj;
        int result = cc::invoke(&CallableWithMembers::method, obj);
        CHECK(result == 100);
        CHECK(obj.call_counter == 1);
    }
}

// =========================================================================================================
// Proxy dereference type tests
// =========================================================================================================

TEST("invoke - proxy dereference type (custom smart-pointer-ish)")
{
    SECTION("member function via proxy")
    {
        S s;
        s.value = 50;
        P proxy{&s};

        int result = cc::invoke(&S::f, proxy, 10);
        CHECK(result == 60);
        CHECK(s.call_count == 1);
    }

    SECTION("member object via proxy")
    {
        MO m{100};

        struct PMO
        {
            MO* p;
            MO& operator*() const { return *p; }
        };
        PMO proxy{&m};

        CHECK(cc::invoke(&MO::x, proxy) == 100);
        cc::invoke(&MO::x, proxy) = 200;
        CHECK(m.x == 200);
    }

    SECTION("const proxy with const method")
    {
        S s;
        s.value = 75;
        P const proxy{&s};

        int result = cc::invoke(&S::f_const, proxy, 25);
        CHECK(result == 100);
    }
}

// =========================================================================================================
// Additional edge cases and comprehensive coverage
// =========================================================================================================

TEST("invoke - comprehensive is_invocable matrix")
{
    SECTION("all callable forms")
    {
        // Lambda
        auto lambda = [](int) { return 1; };
        static_assert(cc::is_invocable<decltype(lambda), int>);

        // Function pointer
        using fp_t = int (*)(int);
        static_assert(cc::is_invocable<fp_t, int>);

        // Functor
        struct Functor
        {
            int operator()(int) { return 1; }
        };
        static_assert(cc::is_invocable<Functor, int>);

        SUCCEED(); // just static checks
    }

    SECTION("member pointer forms with different arg0 types")
    {
        // Member function: obj, ptr, smart ptr
        static_assert(cc::is_invocable<decltype(&S::f), S, int>);
        static_assert(cc::is_invocable<decltype(&S::f), S&, int>);
        static_assert(cc::is_invocable<decltype(&S::f), S*, int>);

        // Member object: obj, ptr, smart ptr
        static_assert(cc::is_invocable<decltype(&MO::x), MO>);
        static_assert(cc::is_invocable<decltype(&MO::x), MO&>);
        static_assert(cc::is_invocable<decltype(&MO::x), MO*>);

        SUCCEED(); // just static checks
    }
}

TEST("invoke - return type preservation")
{
    SECTION("prvalue")
    {
        auto f = []() { return 42; };
        static_assert(std::is_same_v<decltype(cc::invoke(f)), int>);

        SUCCEED(); // just static checks
    }

    SECTION("lvalue reference")
    {
        int x = 5;
        auto f = [&]() -> int& { return x; };
        static_assert(std::is_same_v<decltype(cc::invoke(f)), int&>);

        SUCCEED(); // just static checks
    }

    SECTION("rvalue reference")
    {
        auto f = []() -> int&&
        {
            static int x = 5;
            return cc::move(x);
        };
        static_assert(std::is_same_v<decltype(cc::invoke(f)), int&&>);

        SUCCEED(); // just static checks
    }

    SECTION("const lvalue reference")
    {
        int const x = 5;
        auto f = [&]() -> int const& { return x; };
        static_assert(std::is_same_v<decltype(cc::invoke(f)), int const&>);

        SUCCEED(); // just static checks
    }
}

TEST("invoke - complex scenarios")
{
    SECTION("nested invocations")
    {
        auto outer = [](auto f, int x) { return cc::invoke(f, x); };
        auto inner = [](int x) { return x * 2; };

        CHECK(outer(inner, 5) == 10);
    }

    SECTION("invoke with perfect forwarding chain")
    {
        auto wrapper = [](auto&& f, auto&&... args)
        { return cc::invoke(cc::forward<decltype(f)>(f), cc::forward<decltype(args)>(args)...); };

        auto func = [](int a, int b) { return a + b; };
        CHECK(wrapper(func, 3, 4) == 7);
    }

    SECTION("member function returning reference")
    {
        struct RefReturner
        {
            int value = 0;
            int& get() { return value; }
        };

        RefReturner r;
        r.value = 10;

        static_assert(std::is_same_v<decltype(cc::invoke(&RefReturner::get, r)), int&>);
        cc::invoke(&RefReturner::get, r) = 20;
        CHECK(r.value == 20);
    }
}
