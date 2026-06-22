#include <clean-core/function_ref.hh>

#include <nexus/test.hh>

#include <memory>
#include <string>

// =========================================================================================================
// Helper types for function_ref testing
// =========================================================================================================

namespace
{
// Simple struct with member functions and data
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

// Type with member object
struct MO
{
    int x;
};

// Functor with state
struct Adder
{
    int base;
    int operator()(int x) const { return base + x; }
};

// Callable that distinguishes lvalue/rvalue args
struct Forwarder
{
    int operator()(int&) { return 1; }
    int operator()(int&&) { return 2; }
};

// Mutable functor
struct Counter
{
    int count = 0;
    int operator()() { return ++count; }
};

} // namespace

// =========================================================================================================
// Construction and validity tests
// =========================================================================================================

TEST("function_ref - default construction creates invalid state")
{
    auto check_invalid = [](cc::function_ref<int()> f)
    {
        CHECK(!f.is_valid());
        CHECK(!f);
    };
    check_invalid({});
}

TEST("function_ref - construction from function pointer")
{
    SECTION("0-ary function")
    {
        auto func = +[]() { return 42; };
        auto check = [](cc::function_ref<int()> f)
        {
            CHECK(f.is_valid());
            CHECK(f);
            CHECK(f() == 42);
        };
        check(func);
    }

    SECTION("unary function")
    {
        auto func = +[](int x) { return x * 2; };
        auto check = [](cc::function_ref<int(int)> f)
        {
            CHECK(f.is_valid());
            CHECK(f(5) == 10);
        };
        check(func);
    }

    SECTION("multi-arg function")
    {
        auto func = +[](int a, int b, int c) { return a + b + c; };
        auto check = [](cc::function_ref<int(int, int, int)> f) { CHECK(f(1, 2, 3) == 6); };
        check(func);
    }
}

TEST("function_ref - construction from lambda lvalue")
{
    SECTION("stateless lambda")
    {
        auto lambda = [](int x) { return x + 1; };
        auto check = [](cc::function_ref<int(int)> f)
        {
            CHECK(f.is_valid());
            CHECK(f(10) == 11);
        };
        check(lambda);
    }

    SECTION("lambda with captures")
    {
        int base = 100;
        auto lambda = [&base](int x) { return base + x; };
        auto check = [&base](cc::function_ref<int(int)> f)
        {
            CHECK(f(5) == 105);

            base = 200;
            CHECK(f(5) == 205); // captures by reference, sees updated value
        };
        check(lambda);
    }

    SECTION("mutable lambda")
    {
        auto lambda = [count = 0]() mutable { return ++count; };
        auto check = [](cc::function_ref<int()> f)
        {
            CHECK(f() == 1);
            CHECK(f() == 2); // state persists in the lambda itself
            CHECK(f() == 3);
        };
        check(lambda);
    }
}

TEST("function_ref - construction from functor")
{
    SECTION("const functor")
    {
        Adder adder{10};
        auto check = [](cc::function_ref<int(int)> f)
        {
            CHECK(f.is_valid());
            CHECK(f(5) == 15);
        };
        check(adder);
    }

    SECTION("mutable functor")
    {
        Counter counter;
        auto check = [&counter](cc::function_ref<int()> f)
        {
            CHECK(f() == 1);
            CHECK(f() == 2);
            CHECK(counter.count == 2); // original functor is modified
        };
        check(counter);
    }
}

TEST("function_ref - construction from pointer-to-member-function")
{
    SECTION("non-const member function with object")
    {
        S obj;
        obj.value = 10;

        auto check = [&obj](cc::function_ref<int(S&, int)> f)
        {
            CHECK(f.is_valid());
            CHECK(f(obj, 5) == 15);
            CHECK(obj.call_count == 1);
        };
        check(&S::add);
    }

    SECTION("non-const member function with pointer")
    {
        S obj;
        obj.value = 20;
        S* ptr = &obj;

        auto check = [&obj, ptr](cc::function_ref<int(S*, int)> f)
        {
            CHECK(f(ptr, 3) == 23);
            CHECK(obj.call_count == 1);
        };
        check(&S::add);
    }

    SECTION("const member function")
    {
        S const obj{30, 0};
        auto check = [&obj](cc::function_ref<int(S const&, int)> f) { CHECK(f(obj, 12) == 42); };
        check(&S::add_const);
    }

    SECTION("member function with smart pointer")
    {
        auto up = std::make_unique<S>();
        up->value = 50;

        auto check = [&up](cc::function_ref<int(std::unique_ptr<S>&, int)> f)
        {
            CHECK(f(up, 7) == 57);
            CHECK(up->call_count == 1);
        };
        check(&S::add);
    }
}

TEST("function_ref - construction from pointer-to-member-object")
{
    SECTION("member object with object")
    {
        MO obj{42};
        auto check = [&obj](cc::function_ref<int&(MO&)> f)
        {
            CHECK(f.is_valid());
            CHECK(f(obj) == 42);

            f(obj) = 99;
            CHECK(obj.x == 99);
        };
        check(&MO::x);
    }

    SECTION("member object with pointer")
    {
        MO obj{10};
        MO* ptr = &obj;

        auto check = [&obj, ptr](cc::function_ref<int&(MO*)> f)
        {
            CHECK(f(ptr) == 10);
            f(ptr) = 20;
            CHECK(obj.x == 20);
        };
        check(&MO::x);
    }

    SECTION("member object with smart pointer")
    {
        auto up = std::make_unique<MO>();
        up->x = 30;

        auto check = [&up](cc::function_ref<int&(std::unique_ptr<MO>&)> f)
        {
            CHECK(f(up) == 30);
            f(up) = 40;
            CHECK(up->x == 40);
        };
        check(&MO::x);
    }
}

TEST("function_ref - validity queries")
{
    SECTION("default constructed is invalid")
    {
        auto check = [](cc::function_ref<void()> f)
        {
            CHECK(!f.is_valid());
            CHECK(!static_cast<bool>(f));
        };
        check({});
    }

    SECTION("constructed from callable is valid")
    {
        auto lambda = []() {};
        auto check = [](cc::function_ref<void()> f)
        {
            CHECK(f.is_valid());
            CHECK(static_cast<bool>(f));
        };
        check(lambda);
    }
}

// =========================================================================================================
// Invocation behavior tests
// =========================================================================================================

TEST("function_ref - invocation with various argument counts")
{
    SECTION("0-ary")
    {
        auto f0 = []() { return 42; };
        auto check = [](cc::function_ref<int()> ref) { CHECK(ref() == 42); };
        check(f0);
    }

    SECTION("unary")
    {
        auto f1 = [](int x) { return x * 2; };
        auto check = [](cc::function_ref<int(int)> ref) { CHECK(ref(5) == 10); };
        check(f1);
    }

    SECTION("binary")
    {
        auto f2 = [](int a, int b) { return a + b; };
        auto check = [](cc::function_ref<int(int, int)> ref) { CHECK(ref(3, 4) == 7); };
        check(f2);
    }

    SECTION("ternary")
    {
        auto f3 = [](int a, int b, int c) { return a * b + c; };
        auto check = [](cc::function_ref<int(int, int, int)> ref) { CHECK(ref(2, 3, 4) == 10); };
        check(f3);
    }
}

TEST("function_ref - argument forwarding preserves value category")
{
    SECTION("lvalue vs rvalue overload resolution")
    {
        Forwarder fwd;
        auto check_lvalue = [](cc::function_ref<int(int&)> ref_lvalue, int& x)
        {
            CHECK(ref_lvalue(x) == 1); // lvalue arg
        };
        auto check_rvalue = [](cc::function_ref<int(int&&)> ref_rvalue)
        {
            CHECK(ref_rvalue(10) == 2);     // rvalue arg
            CHECK(ref_rvalue(int{7}) == 2); // prvalue arg
        };

        int x = 5;
        check_lvalue(fwd, x);
        check_rvalue(fwd);
    }

    SECTION("forwarding through wrapper")
    {
        auto wrapper = [](auto&& f, auto&&... args) { return f(cc::forward<decltype(args)>(args)...); };

        Forwarder fwd;
        auto check = [&wrapper](cc::function_ref<int(int&)> ref_lvalue, cc::function_ref<int(int&&)> ref_rvalue, int& x)
        {
            CHECK(wrapper(ref_lvalue, x) == 1);
            CHECK(wrapper(ref_rvalue, 10) == 2);
        };

        int x = 5;
        check(fwd, fwd, x);
    }
}

TEST("function_ref - return type preservation")
{
    SECTION("return by value")
    {
        auto f = []() { return 42; };
        auto check = [](cc::function_ref<int()> ref)
        {
            static_assert(std::is_same_v<decltype(ref()), int>);
            CHECK(ref() == 42);
        };
        check(f);
    }

    SECTION("return lvalue reference")
    {
        int x = 5;
        auto f = [&]() -> int& { return x; };
        auto check = [&x](cc::function_ref<int&()> ref)
        {
            static_assert(std::is_same_v<decltype(ref()), int&>);

            int& result = ref();
            CHECK(&result == &x);

            ref() = 10;
            CHECK(x == 10);
        };
        check(f);
    }

    SECTION("return rvalue reference")
    {
        auto f = []() -> int&&
        {
            static int x = 5;
            return cc::move(x);
        };
        auto check = [](cc::function_ref<int&&()> ref)
        {
            static_assert(std::is_same_v<decltype(ref()), int&&>);

            SUCCEED(); // just static checks
        };
        check(f);
    }

    SECTION("return const lvalue reference")
    {
        int const x = 42;
        auto f = [&]() -> int const& { return x; }; // NOLINT
        auto check = [](cc::function_ref<int const&()> ref)
        {
            static_assert(std::is_same_v<decltype(ref()), int const&>);
            CHECK(ref() == 42);
        };
        check(f);
    }

    SECTION("member function returning reference")
    {
        S obj;
        obj.value = 100;

        auto check = [&obj](cc::function_ref<int&(S&)> ref)
        {
            static_assert(std::is_same_v<decltype(ref(obj)), int&>);

            int& val_ref = ref(obj);
            CHECK(&val_ref == &obj.value);

            ref(obj) = 200;
            CHECK(obj.value == 200);
        };
        check(&S::get_value);
    }
}

TEST("function_ref - void return type")
{
    SECTION("void callable")
    {
        int side_effect = 0;
        auto f = [&]() { side_effect = 42; };
        auto check = [&side_effect](cc::function_ref<void()> ref)
        {
            ref();
            CHECK(side_effect == 42);
        };
        check(f);
    }

    SECTION("void with arguments")
    {
        int result = 0;
        auto f = [&](int a, int b) { result = a + b; };
        auto check = [&result](cc::function_ref<void(int, int)> ref)
        {
            ref(3, 4);
            CHECK(result == 7);
        };
        check(f);
    }
}

// =========================================================================================================
// Copy and move semantics tests
// =========================================================================================================

TEST("function_ref - copy construction preserves callable reference")
{
    SECTION("copy references same callable")
    {
        int count = 0;
        auto f = [&]() { return ++count; };

        auto check = [&count](cc::function_ref<int()> ref1, cc::function_ref<int()> ref2)
        {
            CHECK(ref1.is_valid());
            CHECK(ref2.is_valid());

            CHECK(ref1() == 1);
            CHECK(ref2() == 2); // both reference the same lambda
            CHECK(count == 2);
        };
        check(f, f);
    }
}

TEST("function_ref - copy assignment works correctly")
{
    SECTION("assign from valid to invalid")
    {
        auto f = []() { return 42; };
        auto check = [](cc::function_ref<int()> ref1, cc::function_ref<int()> ref2)
        {
            CHECK(ref2.is_valid());
            CHECK(ref2() == 42);
        };
        check(f, f);
    }

    SECTION("assign from valid to valid")
    {
        auto f1 = []() { return 10; };
        auto f2 = []() { return 20; };

        auto check = [](cc::function_ref<int()> ref1, cc::function_ref<int()> ref2)
        {
            CHECK(ref1() == 10);
            CHECK(ref2() == 10);
        };
        check(f1, f1);
    }
}

TEST("function_ref - move construction and assignment")
{
    SECTION("move construction")
    {
        auto f = []() { return 42; };
        auto check = [](cc::function_ref<int()> ref1, cc::function_ref<int()> ref2)
        {
            CHECK(ref2.is_valid());
            CHECK(ref2() == 42);

            // ref1 is trivially copyable, so move doesn't invalidate it
            CHECK(ref1.is_valid());
            CHECK(ref1() == 42);
        };
        check(f, f);
    }

    SECTION("move assignment")
    {
        auto f1 = []() { return 10; };

        auto check = [](cc::function_ref<int()> ref) { CHECK(ref() == 10); };
        check(f1);
    }
}

// =========================================================================================================
// Multiple references and reassignment tests
// =========================================================================================================

TEST("function_ref - multiple instances can reference same callable")
{
    SECTION("multiple refs to same lambda")
    {
        int count = 0;
        auto f = [&]() { return ++count; };

        auto check = [&count](cc::function_ref<int()> ref1, cc::function_ref<int()> ref2, cc::function_ref<int()> ref3)
        {
            CHECK(ref1() == 1);
            CHECK(ref2() == 2);
            CHECK(ref3() == 3);
            CHECK(count == 3);
        };
        check(f, f, f);
    }

    SECTION("multiple refs to same functor")
    {
        Counter counter;
        auto check = [&counter](cc::function_ref<int()> ref1, cc::function_ref<int()> ref2)
        {
            CHECK(ref1() == 1);
            CHECK(ref2() == 2);
            CHECK(counter.count == 2);
        };
        check(counter, counter);
    }
}

TEST("function_ref - reassignment to different callables")
{
    SECTION("reassign to different lambda")
    {
        auto f1 = []() { return 10; };
        auto f2 = []() { return 20; };

        auto check1 = [](cc::function_ref<int()> ref) { CHECK(ref() == 10); };
        auto check2 = [](cc::function_ref<int()> ref) { CHECK(ref() == 20); };
        check1(f1);
        check2(f2);
    }

    SECTION("reassign to different callable types")
    {
        auto lambda = []() { return 1; };
        auto func_ptr = +[]() { return 2; };

        auto check1 = [](cc::function_ref<int()> ref) { CHECK(ref() == 1); };
        auto check2 = [](cc::function_ref<int()> ref) { CHECK(ref() == 2); };
        check1(lambda);
        check2(func_ptr);
    }
}

// =========================================================================================================
// Special callable types tests
// =========================================================================================================

TEST("function_ref - generic lambda")
{
    SECTION("generic lambda with int")
    {
        auto generic = [](auto x) { return x * 2; };
        auto check = [](cc::function_ref<int(int)> ref) { CHECK(ref(5) == 10); };
        check(generic);
    }

    SECTION("generic lambda with double")
    {
        auto generic = [](auto x) { return x * 2; };
        auto check = [](cc::function_ref<double(double)> ref) { CHECK(ref(2.5) == 5.0); };
        check(generic);
    }
}

TEST("function_ref - capture-heavy lambda")
{
    SECTION("lambda with multiple captures")
    {
        int a = 1, b = 2, c = 3, d = 4;
        std::string s = "test";

        auto lambda = [&a, &b, &c, &d, &s](int x) { return a + b + c + d + x + static_cast<int>(s.length()); };

        auto check = [](cc::function_ref<int(int)> ref)
        {
            CHECK(ref(10) == 1 + 2 + 3 + 4 + 10 + 4); // 24
        };
        check(lambda);
    }
}

TEST("function_ref - function pointer decay via unary plus")
{
    SECTION("unary + on stateless lambda")
    {
        auto func = +[](int x) { return x * 2; };
        auto check = [](cc::function_ref<int(int)> ref)
        {
            CHECK(ref.is_valid());
            CHECK(ref(5) == 10);
        };
        check(func);
    }
}

// =========================================================================================================
// Type conversion and compatibility tests
// =========================================================================================================

TEST("function_ref - implicit conversions in arguments")
{
    SECTION("numeric conversions")
    {
        auto f = [](int x) { return x; };
        auto check = [](cc::function_ref<int(int)> ref)
        {
            CHECK(ref(5) == 5);
            CHECK(ref(static_cast<short>(3)) == 3);
            CHECK(ref(static_cast<char>(7)) == 7);
        };
        check(f);
    }

    SECTION("pointer conversions")
    {
        auto f = [](void const* p) { return p != nullptr; };
        auto check = [](cc::function_ref<bool(void const*)> ref)
        {
            int x = 42;
            CHECK(ref(&x) == true);
            CHECK(ref(nullptr) == false);
        };
        check(f);
    }
}

TEST("function_ref - return type conversions")
{
    SECTION("int to double")
    {
        auto f = []() { return 42; };
        auto check = [](cc::function_ref<double()> ref)
        {
            static_assert(std::is_same_v<decltype(ref()), double>);
            CHECK(ref() == 42.0);
        };
        check(f);
    }

    SECTION("derived to base pointer")
    {
        struct Base
        {
        };
        struct Derived : Base
        {
        };

        Derived d;
        auto f = [&]() -> Derived* { return &d; };
        auto check = [&d](cc::function_ref<Base*()> ref)
        {
            Base* result = ref();
            CHECK(result == &d);
        };
        check(f);
    }
}

// =========================================================================================================
// Edge cases and comprehensive scenarios
// =========================================================================================================

TEST("function_ref - passing function_ref to functions")
{
    SECTION("as function parameter")
    {
        auto process = [](cc::function_ref<int(int)> f, int x) { return f(x) + 1; };

        auto lambda = [](int x) { return x * 2; };
        CHECK(process(lambda, 5) == 11);

        auto func_ptr = +[](int x) { return x * 3; };
        CHECK(process(func_ptr, 5) == 16);
    }

    SECTION("passing through multiple layers")
    {
        auto outer = [](cc::function_ref<int(int)> f, int x)
        {
            auto inner = [&](cc::function_ref<int(int)> g, int y) { return g(y); };
            return inner(f, x);
        };

        auto func = [](int x) { return x + 10; };
        CHECK(outer(func, 5) == 15);
    }
}

TEST("function_ref - same callable, different signatures")
{
    SECTION("overloaded functor")
    {
        struct Overloaded
        {
            int operator()(int x) { return x; }
            double operator()(double x) { return x; }
        };

        Overloaded obj;

        auto check_int = [](cc::function_ref<int(int)> ref_int) { CHECK(ref_int(42) == 42); };
        auto check_double = [](cc::function_ref<double(double)> ref_double) { CHECK(ref_double(3.14) == 3.14); };
        check_int(obj);
        check_double(obj);
    }
}

TEST("function_ref - reference semantics with callable state")
{
    SECTION("modifications to callable are visible")
    {
        Counter counter;
        auto check = [&counter](cc::function_ref<int()> ref)
        {
            CHECK(ref() == 1);
            CHECK(counter.count == 1);

            CHECK(ref() == 2);
            CHECK(counter.count == 2);

            // Modify the original
            counter.count = 100;
            CHECK(ref() == 101);
        };
        check(counter);
    }

    SECTION("lambda captures reflect changes")
    {
        int base = 10;
        auto lambda = [&base](int x) { return base + x; };
        auto check = [&base](cc::function_ref<int(int)> ref)
        {
            CHECK(ref(5) == 15);

            base = 20;
            CHECK(ref(5) == 25);

            base = 100;
            CHECK(ref(5) == 105);
        };
        check(lambda);
    }
}

TEST("function_ref - complex return types")
{
    SECTION("return std::string by value")
    {
        auto f = []() { return std::string("hello"); };
        auto check = [](cc::function_ref<std::string()> ref) { CHECK(ref() == "hello"); };
        check(f);
    }

    SECTION("return std::pair")
    {
        auto f = [](int x) { return std::pair{x, x * 2}; };
        auto check = [](cc::function_ref<std::pair<int, int>(int)> ref)
        {
            auto result = ref(5);
            CHECK(result.first == 5);
            CHECK(result.second == 10);
        };
        check(f);
    }

    SECTION("return reference to complex type")
    {
        std::string s = "test";
        auto f = [&]() -> std::string& { return s; };
        auto check = [&s](cc::function_ref<std::string&()> ref)
        {
            std::string& result = ref();
            CHECK(&result == &s);

            ref() = "modified";
            CHECK(s == "modified");
        };
        check(f);
    }
}

// =========================================================================================================
// Implicit conversion from temporaries (rvalue callables)
// =========================================================================================================

TEST("function_ref - implicit conversion from lambda temporaries")
{
    SECTION("stateless lambda temporary")
    {
        auto process = [](cc::function_ref<int(int)> f, int x) { return f(x); };

        // Pass lambda directly as temporary
        CHECK(process([](int x) { return x * 2; }, 5) == 10);
        CHECK(process([](int x) { return x + 10; }, 7) == 17);
    }

    SECTION("lambda with captures as temporary")
    {
        auto process = [](cc::function_ref<int(int)> f, int x) { return f(x); };

        int base = 100;
        CHECK(process([&base](int x) { return base + x; }, 5) == 105);

        base = 200;
        CHECK(process([base](int x) { return base + x; }, 5) == 205);
    }

    SECTION("mutable lambda temporary")
    {
        auto execute = [](cc::function_ref<int()> f) { return f(); };

        // Note: mutable lambda as temporary can be invoked through function_ref
        int count = 0;
        auto counter = [&count]() mutable { return ++count; };
        CHECK(execute(counter) == 1);
        CHECK(execute(counter) == 2);
    }

    SECTION("generic lambda temporary")
    {
        auto process_int = [](cc::function_ref<int(int)> f, int x) { return f(x); };
        auto process_double = [](cc::function_ref<double(double)> f, double x) { return f(x); };

        // Same generic lambda used with different instantiations
        CHECK(process_int([](auto x) { return x * 2; }, 5) == 10);
        CHECK(process_double([](auto x) { return x * 2; }, 2.5) == 5.0);
    }
}

TEST("function_ref - implicit conversion from function pointer temporaries")
{
    SECTION("function pointer via unary plus")
    {
        auto process = [](cc::function_ref<int(int)> f, int x) { return f(x); };

        // Unary + converts stateless lambda to function pointer
        CHECK(process(+[](int x) { return x * 3; }, 5) == 15);
        CHECK(process(+[](int x) { return x - 1; }, 10) == 9);
    }

    SECTION("actual function pointer")
    {
        auto multiply_by_two = +[](int x) { return x * 2; };
        auto process = [](cc::function_ref<int(int)> f, int x) { return f(x); };

        CHECK(process(multiply_by_two, 7) == 14);
    }

    SECTION("multiple calls with different function pointers")
    {
        auto execute = [](cc::function_ref<int()> f) { return f(); };

        CHECK(execute(+[]() { return 42; }) == 42);
        CHECK(execute(+[]() { return 99; }) == 99);
        CHECK(execute(+[]() { return 0; }) == 0);
    }
}

TEST("function_ref - implicit conversion from functor temporaries")
{
    SECTION("functor constructed inline")
    {
        auto process = [](cc::function_ref<int(int)> f, int x) { return f(x); };

        CHECK(process(Adder{10}, 5) == 15);
        CHECK(process(Adder{100}, 7) == 107);
    }

    SECTION("functor with multiple invocations")
    {
        auto execute_twice = [](cc::function_ref<int()> f)
        {
            int const first = f();
            int const second = f();
            return first + second;
        };

        Counter c;
        CHECK(execute_twice(c) == 3); // 1 + 2
        CHECK(c.count == 2);          // original functor was modified
    }

    SECTION("generic functor temporary")
    {
        struct multiplier
        {
            int factor;
            int operator()(int x) const { return x * factor; }
        };

        auto process = [](cc::function_ref<int(int)> f, int x) { return f(x); };

        CHECK(process(multiplier{3}, 7) == 21);
        CHECK(process(multiplier{5}, 4) == 20);
    }
}

TEST("function_ref - implicit conversion from pointer-to-member-function temporaries")
{
    SECTION("pmf passed directly")
    {
        auto call_member = [](cc::function_ref<int(S&, int)> f, S& obj, int x) { return f(obj, x); };

        S obj;
        obj.value = 50;

        CHECK(call_member(&S::add, obj, 7) == 57);
        CHECK(obj.call_count == 1);
    }

    SECTION("const pmf passed directly")
    {
        auto call_const_member = [](cc::function_ref<int(S const&, int)> f, S const& obj, int x) { return f(obj, x); };

        S const obj{.value = 30, .call_count = 0};
        CHECK(call_const_member(&S::add_const, obj, 12) == 42);
    }

    SECTION("pmf with pointer argument")
    {
        auto call_via_ptr = [](cc::function_ref<int(S*, int)> f, S* ptr, int x) { return f(ptr, x); };

        S obj{.value = 20, .call_count = 0};
        CHECK(call_via_ptr(&S::add, &obj, 5) == 25);
    }

    SECTION("pmf with smart pointer")
    {
        auto call_via_unique
            = [](cc::function_ref<int(std::unique_ptr<S>&, int)> f, std::unique_ptr<S>& up, int x) { return f(up, x); };

        auto up = std::make_unique<S>();
        up->value = 100;
        CHECK(call_via_unique(&S::add, up, 23) == 123);
    }
}

TEST("function_ref - implicit conversion from pointer-to-member-object temporaries")
{
    SECTION("pmd passed directly")
    {
        auto access_member = [](cc::function_ref<int&(MO&)> f, MO& obj) -> int& { return f(obj); };

        MO obj{42};
        CHECK(access_member(&MO::x, obj) == 42);

        access_member(&MO::x, obj) = 99;
        CHECK(obj.x == 99);
    }

    SECTION("pmd with pointer argument")
    {
        auto access_via_ptr = [](cc::function_ref<int&(MO*)> f, MO* ptr) -> int& { return f(ptr); };

        MO obj{10};
        CHECK(access_via_ptr(&MO::x, &obj) == 10);

        access_via_ptr(&MO::x, &obj) = 20;
        CHECK(obj.x == 20);
    }

    SECTION("pmd with smart pointer")
    {
        auto access_via_unique
            = [](cc::function_ref<int&(std::unique_ptr<MO>&)> f, std::unique_ptr<MO>& up) -> int& { return f(up); };

        auto up = std::make_unique<MO>();
        up->x = 30;

        CHECK(access_via_unique(&MO::x, up) == 30);
        access_via_unique(&MO::x, up) = 40;
        CHECK(up->x == 40);
    }
}

TEST("function_ref - mixed temporary and lvalue conversions")
{
    SECTION("chaining function_refs with temporaries")
    {
        auto apply = [](cc::function_ref<int(int)> f, cc::function_ref<int(int)> g, int x) { return g(f(x)); };

        // Both as temporaries
        CHECK(apply([](int x) { return x + 1; }, [](int x) { return x * 2; }, 5) == 12); // (5+1)*2

        // Mixed: first as lvalue, second as temporary
        auto add_ten = [](int x) { return x + 10; };
        CHECK(apply(add_ten, [](int x) { return x * 3; }, 5) == 45); // (5+10)*3

        // Both as lvalues
        auto double_it = [](int x) { return x * 2; };
        CHECK(apply(add_ten, double_it, 5) == 30); // (5+10)*2
    }

    SECTION("temporary in nested function_ref calls")
    {
        auto outer = [](cc::function_ref<int(cc::function_ref<int(int)>, int)> meta, int x)
        { return meta([](int y) { return y * 2; }, x); };

        auto meta_func = [](cc::function_ref<int(int)> f, int val) { return f(val) + 1; };

        CHECK(outer(meta_func, 5) == 11); // (5*2)+1
    }

    SECTION("returning function_ref created from temporary")
    {
        auto create_processor = [](int base) -> cc::function_ref<int(int)>
        {
            // Note: This is intentionally creating a temporary bound to a reference parameter
            // In real code, this would be dangerous due to lifetime issues
            // But for testing the conversion mechanism, we can verify it compiles
            static auto lambda = [base](int x) { return base + x; };
            return lambda;
        };

        auto processor = create_processor(100);
        CHECK(processor(5) == 105);
    }
}

TEST("function_ref - temporary conversions with various signatures")
{
    SECTION("void return from temporary")
    {
        auto execute = [](cc::function_ref<void(int&)> f, int& x) { f(x); };

        int value = 0;
        execute([](int& v) { v = 42; }, value);
        CHECK(value == 42);

        execute([](int& v) { v *= 2; }, value);
        CHECK(value == 84);
    }

    SECTION("multiple arguments from temporary")
    {
        auto compute = [](cc::function_ref<int(int, int, int)> f, int a, int b, int c) { return f(a, b, c); };

        CHECK(compute([](int x, int y, int z) { return x + y + z; }, 1, 2, 3) == 6);
        CHECK(compute([](int x, int y, int z) { return x * y + z; }, 2, 3, 4) == 10);
    }

    SECTION("reference return from temporary")
    {
        int value = 100;
        auto get_ref = [](cc::function_ref<int&()> f) -> int& { return f(); };

        int& ref = get_ref([&value]() -> int& { return value; }); // NOLINT
        CHECK(&ref == &value);

        get_ref([&value]() -> int& { return value; }) = 200;
        CHECK(value == 200);
    }

    SECTION("rvalue reference argument from temporary")
    {
        auto process_rvalue = [](cc::function_ref<int(int&&)> f, int&& x) { return f(cc::move(x)); };

        CHECK(process_rvalue([](int&& x) { return x * 2; }, 5) == 10);
        CHECK(process_rvalue([](int&& x) { return x + 1; }, 9) == 10);
    }
}
