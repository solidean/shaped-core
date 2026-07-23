#include <clean-core/common/utility.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

#include <cstring>

TEST("string - SSO behavior")
{
    SECTION("small strings stay in SSO mode")
    {
        // Small strings up to 39 bytes should not allocate
        cc::string s1;
        CHECK(s1.empty());
        CHECK(s1.size() == 0);

        cc::string s2 = cc::string("short");
        CHECK(s2.size() == 5);

        cc::string s3 = cc::string("12345678901234567890123456789012345678"); // 38 bytes
        CHECK(s3.size() == 38);

        cc::string s4 = cc::string("123456789012345678901234567890123456789"); // 39 bytes (max SSO)
        CHECK(s4.size() == 39);
    }

    SECTION("large strings use heap allocation")
    {
        // 40 bytes should trigger heap allocation
        cc::string s1 = cc::string("1234567890123456789012345678901234567890"); // 40 bytes
        CHECK(s1.size() == 40);

        cc::string s2 = cc::string("this is a very long string that definitely exceeds the SSO capacity limit");
        CHECK(s2.size() > 39);
    }

    SECTION("SSO to heap transition via push_back")
    {
        cc::string s = cc::string("123456789012345678901234567890123456789"); // 39 bytes (at SSO capacity)
        CHECK(s.size() == 39);

        s.push_back('x'); // Should transition to heap
        CHECK(s.size() == 40);
        CHECK(s[39] == 'x');
    }

    SECTION("SSO to heap transition via append")
    {
        cc::string s = cc::string("short");
        CHECK(s.size() == 5);

        // Append enough to exceed SSO capacity
        s.append(cc::string_view{"1234567890123456789012345678901234567890"}); // total 45 bytes
        CHECK(s.size() == 45);
        CHECK(s == cc::string_view{"short1234567890123456789012345678901234567890"});
    }

    SECTION("SSO boundary - exactly 39 bytes")
    {
        cc::string s = cc::string::create_filled(39, 'a');
        CHECK(s.size() == 39);
        for (cc::isize i = 0; i < 39; ++i)
        {
            CHECK(s[i] == 'a');
        }
    }

    SECTION("SSO boundary - exactly 40 bytes")
    {
        cc::string s = cc::string::create_filled(40, 'b');
        CHECK(s.size() == 40);
        for (cc::isize i = 0; i < 40; ++i)
        {
            CHECK(s[i] == 'b');
        }
    }
}

TEST("string - construction")
{
    SECTION("default construction")
    {
        cc::string s;
        CHECK(s.empty());
        CHECK(s.size() == 0);
        CHECK(s.data() != nullptr);
    }

    SECTION("single char construction")
    {
        cc::string s = cc::string('x');
        CHECK(s.size() == 1);
        CHECK(s[0] == 'x');
    }

    SECTION("C-string construction - small")
    {
        cc::string s = cc::string("hello");
        CHECK(s.size() == 5);
        CHECK(s == cc::string_view{"hello"});
    }

    SECTION("C-string construction - large")
    {
        char const* large = "this is a very long string that exceeds SSO capacity for sure";
        cc::string s = cc::string(large);
        CHECK(s.size() == std::strlen(large));
        CHECK(s == cc::string_view{large});
    }

    SECTION("pointer + size construction - small")
    {
        char const* str = "hello world";
        cc::string s = cc::string(str, 5);
        CHECK(s.size() == 5);
        CHECK(s == cc::string_view{"hello"});
    }

    SECTION("pointer + size construction - large")
    {
        char const* str = "this is a very long string that exceeds SSO capacity for sure";
        cc::string s = cc::string(str, cc::isize(std::strlen(str)));
        CHECK(s.size() == std::strlen(str));
        CHECK(s == cc::string_view{str});
    }

    SECTION("pointer range construction - small")
    {
        char const* str = "hello world";
        cc::string s = cc::string(str, str + 5);
        CHECK(s.size() == 5);
        CHECK(s == cc::string_view{"hello"});
    }

    SECTION("pointer range construction - large")
    {
        char const* str = "this is a very long string that exceeds SSO capacity for sure";
        auto const len = std::strlen(str);
        cc::string s = cc::string(str, str + len);
        CHECK(s.size() == len);
        CHECK(s == cc::string_view{str});
    }

    SECTION("string_view construction - small")
    {
        auto const sv = cc::string_view{"hello"};
        cc::string s = cc::string(sv);
        CHECK(s.size() == 5);
        CHECK(s == sv);
    }

    SECTION("string_view construction - large")
    {
        auto const sv = cc::string_view{"this is a very long string that exceeds SSO capacity for sure"};
        cc::string s = cc::string(sv);
        CHECK(s.size() == sv.size());
        CHECK(s == sv);
    }

    SECTION("empty string construction")
    {
        cc::string s1 = cc::string("");
        CHECK(s1.empty());

        cc::string s2 = cc::string(cc::string_view{});
        CHECK(s2.empty());
    }
}

TEST("string - factory methods")
{
    SECTION("create_copy_of - small")
    {
        auto const sv = cc::string_view{"hello"};
        auto s = cc::string::create_copy_of(sv);
        CHECK(s.size() == 5);
        CHECK(s == sv);
    }

    SECTION("create_copy_of - large")
    {
        auto const sv = cc::string_view{"this is a very long string that exceeds SSO capacity"};
        auto s = cc::string::create_copy_of(sv);
        CHECK(s.size() == sv.size());
        CHECK(s == sv);
    }

    SECTION("create_filled - small")
    {
        auto s = cc::string::create_filled(10, 'x');
        CHECK(s.size() == 10);
        for (cc::isize i = 0; i < 10; ++i)
        {
            CHECK(s[i] == 'x');
        }
    }

    SECTION("create_filled - large")
    {
        auto s = cc::string::create_filled(50, 'y');
        CHECK(s.size() == 50);
        for (cc::isize i = 0; i < 50; ++i)
        {
            CHECK(s[i] == 'y');
        }
    }

    SECTION("create_filled - zero size")
    {
        auto s = cc::string::create_filled(0, 'a');
        CHECK(s.empty());
    }

    SECTION("create_uninitialized - small")
    {
        auto s = cc::string::create_uninitialized(10);
        CHECK(s.size() == 10);
        // Fill it to make it valid
        for (cc::isize i = 0; i < 10; ++i)
            s[i] = 'a';
        CHECK(s[0] == 'a');
    }

    SECTION("create_uninitialized - large")
    {
        auto s = cc::string::create_uninitialized(50);
        CHECK(s.size() == 50);
        // Fill it to make it valid
        for (cc::isize i = 0; i < 50; ++i)
            s[i] = 'b';
        CHECK(s[0] == 'b');
    }

    SECTION("create_with_capacity - SSO capacity")
    {
        auto s = cc::string::create_with_capacity(20);
        CHECK(s.empty());
        CHECK(s.size() == 0);
        // Should be in SSO mode since 20 <= 39
        s.append(cc::string_view{"test"});
        CHECK(s == cc::string_view{"test"});
    }

    SECTION("create_with_capacity - at SSO boundary")
    {
        auto s = cc::string::create_with_capacity(39);
        CHECK(s.empty());
        CHECK(s.size() == 0);
        // Should still be in SSO mode
        s.append(cc::string::create_filled(39, 'x'));
        CHECK(s.size() == 39);
    }

    SECTION("create_with_capacity - heap capacity")
    {
        auto s = cc::string::create_with_capacity(100);
        CHECK(s.empty());
        CHECK(s.size() == 0);
        // Should be in heap mode
        // Note: capacity may be >= 100 due to cacheline alignment
        s.append(cc::string_view{"hello world"});
        CHECK(s == cc::string_view{"hello world"});
    }

    SECTION("create_with_capacity - zero capacity")
    {
        auto s = cc::string::create_with_capacity(0);
        CHECK(s.empty());
        CHECK(s.size() == 0);
    }

    SECTION("create_copy_c_str_materialized - small string")
    {
        auto s = cc::string::create_copy_c_str_materialized(cc::string_view{"hello"});
        CHECK(s.size() == 5);
        CHECK(s == cc::string_view{"hello"});
        // Should have null terminator already materialized
        CHECK(s.data()[5] == '\0');
        CHECK(std::strcmp(s.data(), "hello") == 0);
    }

    SECTION("create_copy_c_str_materialized - empty string")
    {
        auto s = cc::string::create_copy_c_str_materialized(cc::string_view{});
        CHECK(s.empty());
        CHECK(s.size() == 0);
        CHECK(s.data()[0] == '\0');
    }

    SECTION("create_copy_c_str_materialized - large string")
    {
        auto const sv = cc::string_view{"this is a very long string that exceeds SSO capacity for sure"};
        auto s = cc::string::create_copy_c_str_materialized(sv);
        CHECK(s.size() == sv.size());
        CHECK(s == sv);
        // Should have null terminator already materialized
        CHECK(s.data()[sv.size()] == '\0');
        CHECK(std::strcmp(s.data(), "this is a very long string that exceeds SSO capacity for sure") == 0);
    }

    SECTION("create_copy_c_str_materialized - at SSO boundary")
    {
        auto const sv = cc::string_view{"123456789012345678901234567890123456789"}; // 39 bytes
        auto s = cc::string::create_copy_c_str_materialized(sv);
        CHECK(s.size() == 39);
        CHECK(s == sv);
        // Should have null terminator
        CHECK(s.data()[39] == '\0');
    }
}

TEST("string - copy semantics")
{
    SECTION("copy constructor - small string")
    {
        cc::string s1 = cc::string("hello");
        cc::string s2 = s1;
        CHECK(s2.size() == 5);
        CHECK(s2 == cc::string_view{"hello"});
        CHECK(s1 == s2);
    }

    SECTION("copy constructor - large string")
    {
        cc::string s1 = cc::string("this is a very long string that exceeds SSO capacity for sure");
        cc::string s2 = s1;
        CHECK(s2.size() == s1.size());
        CHECK(s1 == s2);
        // Different storage for heap strings
        CHECK(s1.data() != s2.data());
    }

    SECTION("copy assignment - small to small")
    {
        cc::string s1 = cc::string("hello");
        cc::string s2 = cc::string("world");
        s2 = s1;
        CHECK(s2 == cc::string_view{"hello"});
    }

    SECTION("copy assignment - large to large")
    {
        cc::string s1 = cc::string("this is a very long string that exceeds SSO capacity");
        cc::string s2 = cc::string("another very long string that also exceeds SSO capacity");
        s2 = s1;
        CHECK(s1 == s2);
    }

    SECTION("copy assignment - small to large")
    {
        cc::string s1 = cc::string("short");
        cc::string s2 = cc::string("this is a very long string that exceeds SSO capacity for sure");
        s2 = s1;
        CHECK(s2 == cc::string_view{"short"});
        CHECK(s2.size() == 5);
    }

    SECTION("copy assignment - large to small")
    {
        cc::string s1 = cc::string("this is a very long string that exceeds SSO capacity for sure");
        cc::string s2 = cc::string("short");
        s2 = s1;
        CHECK(s2 == s1);
    }

    SECTION("self-assignment")
    {
        cc::string s = cc::string("hello");
        s = s;
        CHECK(s == cc::string_view{"hello"});
    }
}

TEST("string - move semantics")
{
    SECTION("move constructor - small string")
    {
        cc::string s1 = cc::string("hello");
        cc::string s2 = cc::move(s1);
        CHECK(s2 == cc::string_view{"hello"});
        CHECK(s1.empty()); // Moved-from state
    }

    SECTION("move constructor - large string")
    {
        cc::string s1 = cc::string("this is a very long string that exceeds SSO capacity for sure");
        auto const* old_ptr = s1.data();
        cc::string s2 = cc::move(s1);
        CHECK(s2 == cc::string_view{"this is a very long string that exceeds SSO capacity for sure"});
        CHECK(s2.data() == old_ptr); // Should reuse the allocation
        CHECK(s1.empty());           // Moved-from state
    }

    SECTION("move assignment - small to small")
    {
        cc::string s1 = cc::string("hello");
        cc::string s2 = cc::string("world");
        s2 = cc::move(s1);
        CHECK(s2 == cc::string_view{"hello"});
        CHECK(s1.empty());
    }

    SECTION("move assignment - large to large")
    {
        cc::string s1 = cc::string("this is a very long string that exceeds SSO capacity");
        auto const* old_ptr = s1.data();
        cc::string s2 = cc::string("another very long string that also exceeds SSO capacity");
        s2 = cc::move(s1);
        CHECK(s2.data() == old_ptr);
        CHECK(s1.empty());
    }

    SECTION("move assignment - small to large")
    {
        cc::string s1 = cc::string("short");
        cc::string s2 = cc::string("this is a very long string that exceeds SSO capacity for sure");
        s2 = cc::move(s1);
        CHECK(s2 == cc::string_view{"short"});
        CHECK(s1.empty());
    }

    SECTION("move assignment - large to small")
    {
        cc::string s1 = cc::string("this is a very long string that exceeds SSO capacity for sure");
        auto const* old_ptr = s1.data();
        cc::string s2 = cc::string("short");
        s2 = cc::move(s1);
        CHECK(s2.data() == old_ptr);
        CHECK(s1.empty());
    }

    SECTION("self-move-assignment")
    {
        cc::string s = cc::string("hello");
        s = cc::move(s);
        CHECK(s == cc::string_view{"hello"});
    }
}

TEST("string - element access")
{
    SECTION("operator[] - small string")
    {
        cc::string s = cc::string("hello");
        CHECK(s[0] == 'h');
        CHECK(s[1] == 'e');
        CHECK(s[2] == 'l');
        CHECK(s[3] == 'l');
        CHECK(s[4] == 'o');
    }

    SECTION("operator[] - large string")
    {
        cc::string s = cc::string("this is a very long string that exceeds SSO capacity for sure");
        CHECK(s[0] == 't');
        CHECK(s[1] == 'h');
        CHECK(s[8] == 'a');
    }

    SECTION("operator[] - mutable")
    {
        cc::string s = cc::string("hello");
        s[0] = 'H';
        CHECK(s == cc::string_view{"Hello"});
    }

    SECTION("data() access")
    {
        cc::string s = cc::string("test");
        CHECK(s.data() != nullptr);
        CHECK(s.data()[0] == 't');
        CHECK(s.data()[1] == 'e');
    }

    SECTION("data() mutable")
    {
        cc::string s = cc::string("test");
        s.data()[0] = 'T';
        CHECK(s == cc::string_view{"Test"});
    }
}

TEST("string - push_back")
{
    SECTION("push_back to empty string")
    {
        cc::string s;
        s.push_back('a');
        CHECK(s.size() == 1);
        CHECK(s[0] == 'a');
    }

    SECTION("push_back to small string")
    {
        cc::string s = cc::string("hello");
        s.push_back('!');
        CHECK(s.size() == 6);
        CHECK(s == cc::string_view{"hello!"});
    }

    SECTION("push_back multiple chars")
    {
        cc::string s;
        s.push_back('a');
        s.push_back('b');
        s.push_back('c');
        CHECK(s == cc::string_view{"abc"});
    }

    SECTION("push_back at SSO boundary")
    {
        cc::string s = cc::string::create_filled(38, 'x');
        s.push_back('y'); // Still SSO (39 bytes)
        CHECK(s.size() == 39);
        CHECK(s[38] == 'y');

        s.push_back('z'); // Should transition to heap (40 bytes)
        CHECK(s.size() == 40);
        CHECK(s[39] == 'z');
    }

    SECTION("push_back to large string")
    {
        cc::string s = cc::string("this is a very long string that exceeds SSO capacity for sure");
        auto const old_size = s.size();
        s.push_back('!');
        CHECK(s.size() == old_size + 1);
        CHECK(s[old_size] == '!');
    }
}

TEST("string - append")
{
    SECTION("append to empty string")
    {
        cc::string s;
        s.append(cc::string_view{"hello"});
        CHECK(s == cc::string_view{"hello"});
    }

    SECTION("append small to small")
    {
        cc::string s = cc::string("hello");
        s.append(cc::string_view{" world"});
        CHECK(s == cc::string_view{"hello world"});
    }

    SECTION("append empty string")
    {
        cc::string s = cc::string("test");
        s.append(cc::string_view{});
        CHECK(s == cc::string_view{"test"});
    }

    SECTION("append causing SSO to heap transition")
    {
        cc::string s = cc::string("short");
        s.append(cc::string_view{"this is a very long string that exceeds SSO capacity"});
        CHECK(s.size() > 39);
        CHECK(s == cc::string_view{"shortthis is a very long string that exceeds SSO capacity"});
    }

    SECTION("append to large string")
    {
        cc::string s = cc::string("this is a very long string that exceeds SSO capacity for sure");
        s.append(cc::string_view{" more text"});
        CHECK(s == cc::string_view{"this is a very long string that exceeds SSO capacity for sure more text"});
    }

    SECTION("append single char")
    {
        cc::string s = cc::string("hello");
        s.append('!');
        CHECK(s == cc::string_view{"hello!"});
    }

    SECTION("operator+= with string_view")
    {
        cc::string s = cc::string("hello");
        s += cc::string_view{" world"};
        CHECK(s == cc::string_view{"hello world"});
    }

    SECTION("operator+= with char")
    {
        cc::string s = cc::string("test");
        s += '!';
        CHECK(s == cc::string_view{"test!"});
    }
}

TEST("string - concatenation")
{
    SECTION("operator+ with string_view")
    {
        cc::string s1 = cc::string("hello");
        auto s2 = s1 + cc::string_view{" world"};
        CHECK(s2 == cc::string_view{"hello world"});
        CHECK(s1 == cc::string_view{"hello"}); // Original unchanged
    }

    SECTION("operator+ with char")
    {
        cc::string s1 = cc::string("test");
        auto s2 = s1 + '!';
        CHECK(s2 == cc::string_view{"test!"});
        CHECK(s1 == cc::string_view{"test"}); // Original unchanged
    }

    SECTION("chained concatenation")
    {
        cc::string s = cc::string("a");
        auto result = s + cc::string_view{"b"} + 'c';
        CHECK(result == cc::string_view{"abc"});
    }
}

TEST("string - clear")
{
    SECTION("clear small string")
    {
        cc::string s = cc::string("hello");
        s.clear();
        CHECK(s.empty());
        CHECK(s.size() == 0);
    }

    SECTION("clear large string")
    {
        cc::string s = cc::string("this is a very long string that exceeds SSO capacity for sure");
        s.clear();
        CHECK(s.empty());
        CHECK(s.size() == 0);
    }

    SECTION("clear and reuse")
    {
        cc::string s = cc::string("hello");
        s.clear();
        s.append(cc::string_view{"world"});
        CHECK(s == cc::string_view{"world"});
    }
}

TEST("string - c_str_materialize")
{
    SECTION("c_str_materialize - small string with room")
    {
        cc::string s = cc::string("hello");
        auto const* cstr = s.c_str_materialize();
        CHECK(std::strcmp(cstr, "hello") == 0);
        CHECK(cstr[5] == '\0');
    }

    SECTION("c_str_materialize - empty string")
    {
        cc::string s;
        auto const* cstr = s.c_str_materialize();
        CHECK(std::strcmp(cstr, "") == 0);
        CHECK(cstr[0] == '\0');
    }

    SECTION("c_str_materialize - SSO at capacity")
    {
        cc::string s = cc::string::create_filled(39, 'a');
        auto const* cstr = s.c_str_materialize();
        CHECK(std::strlen(cstr) == 39);
        // After materialization, should have transitioned to heap
        CHECK(cstr[39] == '\0');
    }

    SECTION("c_str_materialize - large string")
    {
        cc::string s = cc::string("this is a very long string that exceeds SSO capacity for sure");
        auto const* cstr = s.c_str_materialize();
        CHECK(std::strcmp(cstr, "this is a very long string that exceeds SSO capacity for sure") == 0);
    }

    SECTION("c_str_materialize - multiple calls")
    {
        cc::string s = cc::string("test");
        auto const* cstr1 = s.c_str_materialize();
        auto const* cstr2 = s.c_str_materialize();
        CHECK(std::strcmp(cstr1, "test") == 0);
        CHECK(std::strcmp(cstr2, "test") == 0);
    }
}

TEST("string - comparisons")
{
    SECTION("equality with string_view")
    {
        cc::string s = cc::string("hello");
        CHECK(s == cc::string_view{"hello"});
        CHECK(!(s == cc::string_view{"world"}));
    }

    SECTION("equality - different sizes")
    {
        cc::string s = cc::string("hello");
        CHECK(!(s == cc::string_view{"hello world"}));
    }

    SECTION("equality - empty")
    {
        cc::string s;
        CHECK(s == cc::string_view{});
        CHECK(s == cc::string_view{""});
    }

    SECTION("starts_with")
    {
        cc::string s = cc::string("hello world");
        CHECK(s.starts_with(cc::string_view{"hello"}));
        CHECK(s.starts_with(cc::string_view{""}));
        CHECK(!s.starts_with(cc::string_view{"world"}));
    }

    SECTION("ends_with")
    {
        cc::string s = cc::string("hello world");
        CHECK(s.ends_with(cc::string_view{"world"}));
        CHECK(s.ends_with(cc::string_view{""}));
        CHECK(!s.ends_with(cc::string_view{"hello"}));
    }

    SECTION("contains - string_view")
    {
        cc::string s = cc::string("hello world");
        CHECK(s.contains(cc::string_view{"hello"}));
        CHECK(s.contains(cc::string_view{"world"}));
        CHECK(s.contains(cc::string_view{"lo wo"}));
        CHECK(!s.contains(cc::string_view{"xyz"}));
    }

    SECTION("contains - char")
    {
        cc::string s = cc::string("hello");
        CHECK(s.contains('h'));
        CHECK(s.contains('e'));
        CHECK(s.contains('o'));
        CHECK(!s.contains('x'));
    }
}

TEST("string - special cases")
{
    SECTION("string with embedded null bytes")
    {
        char const data[] = {'a', 'b', '\0', 'c', 'd'};
        cc::string s = cc::string(data, 5);
        CHECK(s.size() == 5);
        CHECK(s[0] == 'a');
        CHECK(s[2] == '\0');
        CHECK(s[4] == 'd');
    }

    SECTION("empty string behavior")
    {
        cc::string s;
        CHECK(s.empty());
        CHECK(s.size() == 0);
        CHECK(s == cc::string_view{});
    }

    SECTION("single character string")
    {
        cc::string s = cc::string('x');
        CHECK(s.size() == 1);
        CHECK(s[0] == 'x');
        CHECK(s == cc::string_view{"x"});
    }

    SECTION("conversion to string_view")
    {
        cc::string s = cc::string("hello");
        cc::string_view sv = s;
        CHECK(sv.data() == s.data());
        CHECK(sv.size() == s.size());
    }

    SECTION("repeated operations")
    {
        cc::string s;
        for (int i = 0; i < 50; ++i)
        {
            s.push_back('a');
        }
        CHECK(s.size() == 50);
        for (cc::isize i = 0; i < 50; ++i)
        {
            CHECK(s[i] == 'a');
        }
    }
}

TEST("string - string_view read forwarding")
{
    cc::string const s = cc::string("hello world");

    SECTION("front / back")
    {
        CHECK(s.front() == 'h');
        CHECK(s.back() == 'd');
    }

    SECTION("compare")
    {
        CHECK(s.compare("hello world") == 0);
        CHECK(s.compare("hello") > 0);
        CHECK(s.compare("z") < 0);
    }

    SECTION("find")
    {
        CHECK(s.find("world") == 6);
        CHECK(s.find('o') == 4);
        CHECK(s.find('o', 5) == 7);
        CHECK(s.find("xyz") == -1);
    }

    SECTION("rfind")
    {
        CHECK(s.rfind('o') == 7);
        CHECK(s.rfind("o") == 7);
        CHECK(s.rfind("l") == 9);
        CHECK(s.rfind("xyz") == -1);
    }
}

TEST("string - subview and substring")
{
    SECTION("subview aliases the string")
    {
        cc::string const s = cc::string("hello world");
        auto const v = s.subview({.offset = 6, .size = 5});
        CHECK(v == cc::string_view{"world"});
        CHECK(v.data() == s.data() + 6);

        CHECK(s.subview(6) == cc::string_view{"world"});
        CHECK(s.subview({.start = 0, .end = 5}) == cc::string_view{"hello"});
    }

    SECTION("substring is an owning copy (SSO)")
    {
        cc::string const s = cc::string("hello world");
        auto const sub = s.substring({.offset = 0, .size = 5});
        CHECK(sub == cc::string_view{"hello"});
        CHECK(sub.data() != s.data());

        CHECK(s.substring(6) == cc::string_view{"world"});
        CHECK(s.substring({.start = 6, .end = 11}) == cc::string_view{"world"});
    }

    SECTION("substring of a heap string")
    {
        cc::string const s = cc::string("the quick brown fox jumps over the lazy dog");
        CHECK(s.size() > 39);
        CHECK(s.substring({.start = 4, .end = 9}) == cc::string_view{"quick"});
        CHECK(s.substring(40) == cc::string_view{"dog"});
    }
}

TEST("string - replace_all char")
{
    SECTION("replaces every occurrence in place")
    {
        cc::string s = cc::string("a.b.c.d");
        auto const n = s.replace_all('.', '/');
        CHECK(n == 3);
        CHECK(s == cc::string_view{"a/b/c/d"});
    }

    SECTION("no match")
    {
        cc::string s = cc::string("abc");
        CHECK(s.replace_all('x', 'y') == 0);
        CHECK(s == cc::string_view{"abc"});
    }
}

TEST("string - replace_all string_view")
{
    SECTION("equal-size replacement")
    {
        cc::string s = cc::string("a.b.c");
        auto const n = s.replace_all(".", "-");
        CHECK(n == 2);
        CHECK(s == cc::string_view{"a-b-c"});
    }

    SECTION("growing replacement")
    {
        cc::string s = cc::string("a.b.c");
        auto const n = s.replace_all(".", "<>");
        CHECK(n == 2);
        CHECK(s == cc::string_view{"a<>b<>c"});
    }

    SECTION("shrinking replacement")
    {
        cc::string s = cc::string("aXXbXXc");
        auto const n = s.replace_all("XX", "-");
        CHECK(n == 2);
        CHECK(s == cc::string_view{"a-b-c"});
    }

    SECTION("growing into heap")
    {
        cc::string s = cc::string("x.x.x.x.x.x.x.x.x.x");
        auto const n = s.replace_all(".", "____");
        CHECK(n == 9);
        CHECK(s.size() > 39);
        CHECK(s == cc::string_view{"x____x____x____x____x____x____x____x____x____x"});
    }

    SECTION("non-overlapping matches")
    {
        cc::string s = cc::string("aaaa");
        auto const n = s.replace_all("aa", "b");
        CHECK(n == 2);
        CHECK(s == cc::string_view{"bb"});
    }

    SECTION("no match leaves string unchanged")
    {
        cc::string s = cc::string("hello");
        CHECK(s.replace_all("xyz", "abc") == 0);
        CHECK(s == cc::string_view{"hello"});
    }

    SECTION("empty from is a no-op")
    {
        cc::string s = cc::string("hello");
        CHECK(s.replace_all("", "x") == 0);
        CHECK(s == cc::string_view{"hello"});
    }

    SECTION("deletion via empty to")
    {
        cc::string s = cc::string("a.b.c");
        auto const n = s.replace_all(".", "");
        CHECK(n == 2);
        CHECK(s == cc::string_view{"abc"});
    }
}

TEST("string - replace_first / replace_last")
{
    SECTION("char overloads")
    {
        cc::string s = cc::string("a.b.c");
        CHECK(s.replace_first('.', '/'));
        CHECK(s == cc::string_view{"a/b.c"});
        CHECK(s.replace_last('.', '/'));
        CHECK(s == cc::string_view{"a/b/c"});
        CHECK(!s.replace_first('x', 'y'));
    }

    SECTION("string_view overloads")
    {
        cc::string s = cc::string("ab.ab.ab");
        CHECK(s.replace_first("ab", "X"));
        CHECK(s == cc::string_view{"X.ab.ab"});
        CHECK(s.replace_last("ab", "Y"));
        CHECK(s == cc::string_view{"X.ab.Y"});
        CHECK(!s.replace_first("zz", "q"));
        CHECK(!s.replace_first("", "q"));
    }
}

TEST("string - replace range")
{
    SECTION("offset_size, equal size")
    {
        cc::string s = cc::string("hello world");
        s.replace({.offset = 6, .size = 5}, "there");
        CHECK(s == cc::string_view{"hello there"});
    }

    SECTION("offset_size, growing")
    {
        cc::string s = cc::string("hello world");
        s.replace({.offset = 0, .size = 5}, "greetings");
        CHECK(s == cc::string_view{"greetings world"});
    }

    SECTION("start_end, shrinking")
    {
        cc::string s = cc::string("hello world");
        s.replace({.start = 0, .end = 5}, "hi");
        CHECK(s == cc::string_view{"hi world"});
    }

    SECTION("insertion via empty range")
    {
        cc::string s = cc::string("ac");
        s.replace({.offset = 1, .size = 0}, "b");
        CHECK(s == cc::string_view{"abc"});
    }

    SECTION("growing into heap")
    {
        cc::string s = cc::string("short");
        s.replace({.start = 0, .end = 5}, "this is a considerably longer replacement string");
        CHECK(s.size() > 39);
        CHECK(s == cc::string_view{"this is a considerably longer replacement string"});
    }

    SECTION("replace whole string")
    {
        cc::string s = cc::string("abc");
        s.replace({.offset = 0, .size = 3}, "xyz");
        CHECK(s == cc::string_view{"xyz"});
    }
}

TEST("string - as_span / as_bytes")
{
    SECTION("spans cover exactly the content, no terminator")
    {
        cc::string s = cc::string("hello");
        auto const chars = s.as_span();
        static_assert(std::is_same_v<decltype(chars), cc::span<char const> const>);
        CHECK(chars.data() == s.data());
        CHECK(chars.size() == 5);

        auto const bytes = s.as_bytes();
        static_assert(std::is_same_v<decltype(bytes), cc::span<cc::byte const> const>);
        CHECK(bytes.size() == 5);
        CHECK(bytes[0] == cc::byte('h'));
    }

    SECTION("as_mutable_span writes through")
    {
        cc::string s = cc::string("hello");
        s.as_mutable_span()[0] = 'H';
        CHECK(s == cc::string_view{"Hello"});
    }

    SECTION("as_mutable_bytes writes through")
    {
        cc::string s = cc::string("hello");
        s.as_mutable_bytes()[4] = cc::byte('O');
        CHECK(s == cc::string_view{"hellO"});
    }

    SECTION("long (heap) string spans its content")
    {
        cc::string s = cc::string("this is a very long string that exceeds SSO capacity for sure");
        CHECK(s.as_span().size() == s.size());
        CHECK(s.as_bytes().size() == s.size());
    }
}

TEST("string - resize and capacity")
{
    auto const all_equal = [](cc::string const& s, cc::isize begin, cc::isize end, char c)
    {
        for (cc::isize i = begin; i < end; ++i)
            if (s[i] != c)
                return false;
        return true;
    };

    SECTION("resize_to_uninitialized grows within SSO, preserving existing bytes")
    {
        cc::string s = cc::string("abc");
        s.resize_to_uninitialized(10);
        CHECK(s.is_small());
        CHECK(s.size() == 10);
        CHECK(s[0] == 'a');
        CHECK(s[1] == 'b');
        CHECK(s[2] == 'c');
    }

    SECTION("resize_to_filled grows and shrinks within SSO")
    {
        cc::string s = cc::string("abc");
        s.resize_to_filled(6, 'x');
        CHECK(s.is_small());
        CHECK(s == cc::string_view{"abcxxx"});

        s.resize_to_filled(3, 'x');
        CHECK(s == cc::string_view{"abc"});
    }

    SECTION("resize_to_defaulted zero-fills new bytes")
    {
        cc::string s = cc::string("ab");
        s.resize_to_defaulted(4);
        CHECK(s.size() == 4);
        CHECK(s[2] == '\0');
        CHECK(s[3] == '\0');
    }

    SECTION("resize_to_filled SSO->heap preserves existing, fills the tail")
    {
        cc::string s = cc::string("abc");
        s.resize_to_filled(50, 'y');
        CHECK(!s.is_small());
        CHECK(s.size() == 50);
        CHECK(s[0] == 'a');
        CHECK(s[1] == 'b');
        CHECK(s[2] == 'c');
        CHECK(all_equal(s, 3, 50, 'y'));
    }

    SECTION("resize_to_uninitialized SSO->heap preserves existing bytes")
    {
        cc::string s = cc::string("hello");
        s.resize_to_uninitialized(64);
        CHECK(!s.is_small());
        CHECK(s.size() == 64);
        CHECK(s.subview({.start = cc::isize(0), .end = cc::isize(5)}) == cc::string_view{"hello"});
    }

    SECTION("resize_down_to on a heap string stays heap")
    {
        cc::string s = cc::string::create_filled(50, 'z');
        CHECK(!s.is_small());
        s.resize_down_to(10);
        CHECK(!s.is_small()); // resize never demotes the storage mode
        CHECK(s.size() == 10);
        CHECK(all_equal(s, 0, 10, 'z'));
    }

    SECTION("clear_resize_to_filled SSO->heap discards existing content")
    {
        cc::string s = cc::string("abc");
        s.clear_resize_to_filled(50, 'q');
        CHECK(!s.is_small());
        CHECK(s.size() == 50);
        CHECK(all_equal(s, 0, 50, 'q'));
    }

    SECTION("clear_resize_to_defaulted zero-fills")
    {
        cc::string s = cc::string("abc");
        s.clear_resize_to_defaulted(5);
        CHECK(s.size() == 5);
        CHECK(all_equal(s, 0, 5, '\0'));
    }

    SECTION("reserve_back stays small when the result fits inline")
    {
        cc::string s;
        s.reserve_back(20);
        CHECK(s.is_small());
        CHECK(s.empty());
        s.append(cc::string_view{"test"});
        CHECK(s == cc::string_view{"test"});
    }

    SECTION("reserve_back materializes to heap beyond SSO capacity")
    {
        cc::string s;
        s.reserve_back(100);
        CHECK(!s.is_small());
        CHECK(s.empty());
        s.append(cc::string_view{"hello world"});
        CHECK(s == cc::string_view{"hello world"});
    }

    SECTION("reserve_back_exact behaves like reserve_back for correctness")
    {
        cc::string s = cc::string("abc");
        s.reserve_back_exact(200);
        CHECK(!s.is_small());
        CHECK(s == cc::string_view{"abc"});
    }

    SECTION("capacity queries in SSO mode")
    {
        cc::string s = cc::string("abc");
        CHECK(s.is_small());
        CHECK(s.capacity_front() == 0);
        CHECK(s.capacity_back() == 39 - 3); // small_capacity - size
    }

    SECTION("reserve_front materializes a small string to heap, preserving content")
    {
        cc::string s = cc::string("abc");
        s.reserve_front(10);
        CHECK(!s.is_small()); // SSO cannot represent a front offset, so it always allocates
        CHECK(s == cc::string_view{"abc"});
        CHECK(s.size() == 3);
        CHECK(s.capacity_front() == 10);    // exactly the requested front slack
        CHECK(s.capacity_back() >= 39 - 3); // SSO-equivalent back room preserved (>= due to alignment)
    }

    SECTION("reserve_front(0) on a small string is a no-op")
    {
        cc::string s = cc::string("abc");
        s.reserve_front(0);
        CHECK(s.is_small());
        CHECK(s == cc::string_view{"abc"});
        CHECK(s.capacity_front() == 0);
    }

    SECTION("reserve_front_exact leaves no back slack beyond alignment")
    {
        cc::string s = cc::string("abc");
        s.reserve_front_exact(8);
        CHECK(!s.is_small());
        CHECK(s == cc::string_view{"abc"});
        CHECK(s.capacity_front() == 8);
    }

    SECTION("reserve_front on a heap string leaves content intact")
    {
        cc::string s = cc::string::create_filled(50, 'a');
        CHECK(!s.is_small());
        s.reserve_front(16);
        CHECK(!s.is_small());
        CHECK(s.size() == 50);
        CHECK(all_equal(s, 0, 50, 'a'));
    }

    SECTION("shrink_to_fit demotes to SSO when the content fits inline")
    {
        cc::string s = cc::string::create_filled(100, 'a');
        s.resize_down_to(5);
        CHECK(!s.is_small()); // still heap after the shrink
        s.shrink_to_fit();
        CHECK(s.is_small()); // reallocation was due anyway, so it returned to SSO
        CHECK(s == cc::string_view{"aaaaa"});
    }

    SECTION("shrink_to_fit stays heap when the content exceeds SSO capacity")
    {
        cc::string s = cc::string::create_filled(100, 'a');
        s.resize_down_to(50);
        s.shrink_to_fit();
        CHECK(!s.is_small());
        CHECK(s.size() == 50);
        CHECK(all_equal(s, 0, 50, 'a'));
    }

    SECTION("shrink_to_fit leaves an already-tight heap string untouched")
    {
        cc::string s = cc::string::create_filled(50, 'a');
        CHECK(!s.is_small());
        s.shrink_to_fit();
        CHECK(!s.is_small());
        CHECK(s.size() == 50);
        CHECK(all_equal(s, 0, 50, 'a'));
    }

    SECTION("SSO boundary: 39 stays small, 40 materializes")
    {
        cc::string s;
        s.resize_to_uninitialized(39);
        CHECK(s.is_small());
        s.resize_to_uninitialized(40);
        CHECK(!s.is_small());
    }

    SECTION("resize down to zero empties the string")
    {
        cc::string s = cc::string("abc");
        s.resize_down_to(0);
        CHECK(s.empty());
    }
}
