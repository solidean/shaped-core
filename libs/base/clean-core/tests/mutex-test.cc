#include <clean-core/mutex.hh>
#include <clean-core/string.hh>
#include <clean-core/to_string.hh>
#include <clean-core/vector.hh>

#include <nexus/test.hh>


TEST("mutex - construction")
{
    SECTION("default construction")
    {
        auto m = cc::mutex<int>{};
        auto value = m.lock([](int const& val) { return val; });
        CHECK(value == 0);
    }

    SECTION("construction with initial value")
    {
        auto m = cc::mutex<int>{42};
        auto value = m.lock([](int const& val) { return val; });
        CHECK(value == 42);
    }

    SECTION("construction with multiple arguments")
    {
        auto m = cc::mutex<cc::string>{"hello"};
        auto value = m.lock([](cc::string const& val) { return val; });
        CHECK(value == "hello");
    }
}

TEST("mutex - lock with modification")
{
    SECTION("modify value")
    {
        auto m = cc::mutex<int>{0};
        m.lock([](int& val) { val = 42; });
        auto value = m.lock([](int const& val) { return val; });
        CHECK(value == 42);
    }

    SECTION("increment value")
    {
        auto m = cc::mutex<int>{0};
        m.lock([](int& val) { ++val; });
        m.lock([](int& val) { ++val; });
        m.lock([](int& val) { ++val; });
        auto value = m.lock([](int const& val) { return val; });
        CHECK(value == 3);
    }

    SECTION("modify string")
    {
        auto m = cc::mutex<cc::string>{"hello"};
        m.lock([](cc::string& val) { val += " world"; });
        auto value = m.lock([](cc::string const& val) { return val; });
        CHECK(value == "hello world");
    }
}

TEST("mutex - lock with return values")
{
    SECTION("return value")
    {
        auto m = cc::mutex<int>{42};
        auto value = m.lock([](int const& val) { return val; });
        CHECK(value == 42);
    }

    SECTION("return modified value")
    {
        auto m = cc::mutex<int>{42};
        auto value = m.lock([](int& val) { return val++; });
        CHECK(value == 42);
        auto new_value = m.lock([](int const& val) { return val; });
        CHECK(new_value == 43);
    }

    SECTION("return computed value")
    {
        auto m = cc::mutex<int>{10};
        auto doubled = m.lock([](int const& val) { return val * 2; });
        CHECK(doubled == 20);
    }

    SECTION("return different type")
    {
        auto m = cc::mutex<int>{42};
        auto as_string = m.lock([](int const& val) { return cc::to_string(val); });
        CHECK(as_string == "42");
    }

    SECTION("void return")
    {
        auto m = cc::mutex<int>{0};
        m.lock([](int& val) { val = 99; });
        CHECK(m.lock([](int const& val) { return val; }) == 99);
    }
}

TEST("mutex - const access")
{
    SECTION("const lambda")
    {
        auto m = cc::mutex<int>{42};
        auto value = m.lock([](int const& val) { return val; });
        CHECK(value == 42);
    }

    SECTION("read-only operations")
    {
        auto m = cc::mutex<cc::string>{"test"};
        auto length = m.lock([](cc::string const& val) { return val.size(); });
        CHECK(length == 4);
    }
}

TEST("mutex - try_lock success")
{
    SECTION("try_lock with return value")
    {
        auto m = cc::mutex<int>{42};
        auto result = m.try_lock([](int const& val) { return val; });
        REQUIRE(result.has_value());
        CHECK(result.value() == 42);
    }

    SECTION("try_lock with modification and return")
    {
        auto m = cc::mutex<int>{10};
        auto result = m.try_lock([](int& val) { return val++; });
        REQUIRE(result.has_value());
        CHECK(result.value() == 10);
        auto new_value = m.lock([](int const& val) { return val; });
        CHECK(new_value == 11);
    }

    SECTION("try_lock with void function")
    {
        auto m = cc::mutex<int>{0};
        auto acquired = m.try_lock([](int& val) { val = 99; });
        CHECK(acquired == true);
        auto value = m.lock([](int const& val) { return val; });
        CHECK(value == 99);
    }

    SECTION("try_lock void returns bool")
    {
        auto m = cc::mutex<int>{42};
        bool success = m.try_lock([](int&) {});
        CHECK(success);
    }

    SECTION("try_lock returning different type")
    {
        auto m = cc::mutex<int>{42};
        auto result = m.try_lock([](int const& val) { return cc::to_string(val); });
        REQUIRE(result.has_value());
        CHECK(result.value() == "42");
    }
}

TEST("mutex - sequential operations")
{
    SECTION("multiple lock calls")
    {
        auto m = cc::mutex<int>{0};

        m.lock([](int& val) { val += 1; });
        m.lock([](int& val) { val += 2; });
        m.lock([](int& val) { val += 3; });

        auto total = m.lock([](int const& val) { return val; });
        CHECK(total == 6);
    }

    SECTION("mix lock and try_lock")
    {
        auto m = cc::mutex<int>{0};

        m.lock([](int& val) { val = 10; });
        auto result = m.try_lock(
            [](int& val)
            {
                val *= 2;
                return val;
            });
        REQUIRE(result.has_value());
        CHECK(result.value() == 20);

        auto final_value = m.lock([](int const& val) { return val; });
        CHECK(final_value == 20);
    }

    SECTION("chained computations")
    {
        auto m = cc::mutex<int>{5};

        auto doubled = m.lock(
            [](int& val)
            {
                val *= 2;
                return val;
            });
        CHECK(doubled == 10);

        auto plus_ten = m.lock(
            [](int& val)
            {
                val += 10;
                return val;
            });
        CHECK(plus_ten == 20);

        auto current = m.lock([](int const& val) { return val; });
        CHECK(current == 20);
    }
}

TEST("mutex - complex types")
{
    SECTION("struct protected by mutex")
    {
        struct point
        {
            int x = 0;
            int y = 0;
        };

        auto m = cc::mutex<point>{{10, 20}};

        m.lock(
            [](point& p)
            {
                p.x += 5;
                p.y += 10;
            });

        auto sum = m.lock([](point const& p) { return p.x + p.y; });
        CHECK(sum == 45);
    }

    SECTION("vector protected by mutex")
    {
        auto m = cc::mutex<cc::vector<int>>{};

        m.lock(
            [](cc::vector<int>& vec)
            {
                vec.push_back(1);
                vec.push_back(2);
                vec.push_back(3);
            });

        auto size = m.lock([](cc::vector<int> const& vec) { return vec.size(); });
        CHECK(size == 3);

        auto sum = m.lock(
            [](cc::vector<int> const& vec)
            {
                int total = 0;
                for (auto val : vec)
                    total += val;
                return total;
            });
        CHECK(sum == 6);
    }
}

TEST("mutex - lambda capture")
{
    SECTION("capture external variable")
    {
        auto m = cc::mutex<int>{10};
        int multiplier = 5;

        auto result = m.lock([multiplier](int const& val) { return val * multiplier; });
        CHECK(result == 50);
    }

    SECTION("modify captured variable")
    {
        auto m = cc::mutex<int>{3};
        int counter = 0;

        m.lock([&counter](int const& val) { counter = val * 2; });

        CHECK(counter == 6);
    }
}
