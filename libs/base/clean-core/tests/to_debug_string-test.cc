#include <clean-core/array.hh>
#include <clean-core/optional.hh>
#include <clean-core/string.hh>
#include <clean-core/to_debug_string.hh>
#include <clean-core/vector.hh>

#include <nexus/test.hh>

#include <array>
#include <list>
#include <tuple>
#include <vector>


// =========================================================================================================
// Helper types for testing dispatch priorities
// =========================================================================================================

namespace
{
// Type with both ADL to_string AND iterability
struct HasAdlAndIterable
{
    cc::vector<int> data = {10, 20, 30};

    auto begin() const { return data.begin(); }
    auto end() const { return data.end(); }
};

cc::string to_string(HasAdlAndIterable const&)
{
    return "ADL_to_string";
}

// Type with member to_string() AND iterability
struct HasMemberAndIterable
{
    cc::vector<int> data = {40, 50};

    cc::string to_string() const { return "member_to_string"; }
    auto begin() const { return data.begin(); }
    auto end() const { return data.end(); }
};

// Type with only member to_string()
struct HasOnlyMember
{
    cc::string to_string() const { return "only_member"; }
};

// Opaque struct for memory dump fallback
struct OpaqueType
{
    uint32_t a;
    uint16_t b;
    uint8_t c;
};

// Empty struct for zero-size test
struct EmptyStruct
{
};

// Type with to_string for heterogeneous tuple test
struct CustomStringable
{
    int value;
    cc::string to_string() const { return cc::to_string(value) + "_custom"; }
};
} // namespace

// =========================================================================================================
// ADL and member dispatch priority tests
// =========================================================================================================

TEST("to_debug_string - ADL to_string takes precedence over iteration")
{
    HasAdlAndIterable obj;
    auto result = cc::to_debug_string(obj);

    // Should use ADL to_string, not iterate
    CHECK(result == "ADL_to_string");
}

TEST("to_debug_string - member to_string() is second priority over iteration")
{
    HasMemberAndIterable obj;
    auto result = cc::to_debug_string(obj);

    // Should use member to_string, not iterate
    CHECK(result == "member_to_string");
}

TEST("to_debug_string - member to_string() works when only option")
{
    HasOnlyMember obj;
    auto result = cc::to_debug_string(obj);

    CHECK(result == "only_member");
}

// =========================================================================================================
// Iterable formatting tests
// =========================================================================================================

TEST("to_debug_string - empty container formats as []")
{
    std::vector<int> empty;
    auto result = cc::to_debug_string(empty);

    CHECK(result == "[]");
}

TEST("to_debug_string - single element container")
{
    std::vector<int> single = {42};
    auto result = cc::to_debug_string(single);

    CHECK(result == "[42]");
}

TEST("to_debug_string - multiple element container")
{
    std::vector<int> multi = {1, 2, 3};
    auto result = cc::to_debug_string(multi);

    CHECK(result == "[1, 2, 3]");
}

TEST("to_debug_string - container with strings")
{
    std::vector<cc::string> strings = {"hello", "world"};
    auto result = cc::to_debug_string(strings);

    // Strings are now wrapped in quotes
    CHECK(result == "[\"hello\", \"world\"]");
}

TEST("to_debug_string - different container types")
{
    SECTION("std::list")
    {
        std::list<int> lst = {10, 20, 30};
        auto result = cc::to_debug_string(lst);
        CHECK(result == "[10, 20, 30]");
    }

    SECTION("cc::vector")
    {
        cc::vector<int> vec = {5, 15, 25};
        auto result = cc::to_debug_string(vec);
        CHECK(result == "[5, 15, 25]");
    }
}

// =========================================================================================================
// Nested collection tests
// =========================================================================================================

TEST("to_debug_string - nested vectors")
{
    std::vector<std::vector<int>> nested = {{1, 2}, {3, 4}};
    auto result = cc::to_debug_string(nested);

    CHECK(result == "[[1, 2], [3, 4]]");
}

TEST("to_debug_string - triple nested collections")
{
    std::vector<std::vector<std::vector<int>>> triple = {{{1, 2}}, {{3}}};
    auto result = cc::to_debug_string(triple);

    CHECK(result == "[[[1, 2]], [[3]]]");
}

TEST("to_debug_string - list of arrays")
{
    std::list<std::array<int, 2>> hybrid;
    hybrid.push_back({10, 20});
    hybrid.push_back({30, 40});
    auto result = cc::to_debug_string(hybrid);

    CHECK(result == "[[10, 20], [30, 40]]");
}

TEST("to_debug_string - list of tuples")
{
    std::list<std::tuple<int, int>> hybrid;
    hybrid.push_back({10, 20});
    hybrid.push_back({30, 40});
    auto result = cc::to_debug_string(hybrid);

    CHECK(result == "[(10, 20), (30, 40)]");
}

TEST("to_debug_string - nested with empty containers")
{
    std::vector<std::vector<int>> nested = {{}, {1}, {}};
    auto result = cc::to_debug_string(nested);

    CHECK(result == "[[], [1], []]");
}

// =========================================================================================================
// Tuple-like formatting tests
// =========================================================================================================

TEST("to_debug_string - empty tuple")
{
    std::tuple<> empty;
    auto result = cc::to_debug_string(empty);

    CHECK(result == "()");
}

TEST("to_debug_string - single element tuple")
{
    std::tuple<int> single{42};
    auto result = cc::to_debug_string(single);

    CHECK(result == "(42)");
}

TEST("to_debug_string - pair")
{
    std::pair<int, int> p{10, 20};
    auto result = cc::to_debug_string(p);

    CHECK(result == "(10, 20)");
}

TEST("to_debug_string - multi-element tuple")
{
    std::tuple<int, int, int> triple{1, 2, 3};
    auto result = cc::to_debug_string(triple);

    CHECK(result == "(1, 2, 3)");
}

TEST("to_debug_string - std::array as collection")
{
    std::array<int, 3> arr = {5, 10, 15};
    auto result = cc::to_debug_string(arr);

    CHECK(result == "[5, 10, 15]");
}

TEST("to_debug_string - tuple with same size as array")
{
    std::tuple<int, int, int> tup{5, 10, 15};
    auto result = cc::to_debug_string(tup);

    CHECK(result == "(5, 10, 15)");
}

TEST("to_debug_string - large tuple")
{
    auto large = std::make_tuple(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    auto result = cc::to_debug_string(large);

    CHECK(result == "(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)");
}

// =========================================================================================================
// Heterogeneous tuple tests
// =========================================================================================================

TEST("to_debug_string - heterogeneous tuple with different dispatch paths")
{
    std::tuple<int, cc::string, std::vector<int>> mixed{42, "hello", {1, 2, 3}};
    auto result = cc::to_debug_string(mixed);

    // int → to_string, string → wrapped in quotes, vector → iterable
    CHECK(result == "(42, \"hello\", [1, 2, 3])");
}

TEST("to_debug_string - tuple with custom stringable type")
{
    std::tuple<int, CustomStringable, int> custom{10, CustomStringable{99}, 20};
    auto result = cc::to_debug_string(custom);

    CHECK(result == "(10, 99_custom, 20)");
}

TEST("to_debug_string - nested tuple and vector")
{
    std::tuple<std::vector<int>, int> nested{{5, 6}, 7};
    auto result = cc::to_debug_string(nested);

    CHECK(result == "([5, 6], 7)");
}

// =========================================================================================================
// Memory dump fallback tests
// =========================================================================================================

TEST("to_debug_string - opaque struct produces hex dump")
{
    OpaqueType obj{0x12345678, 0xABCD, 0xEF};
    auto result = cc::to_debug_string(obj);

    // Should contain 0x as substring and underscores as alignment separators
    CHECK(result.contains("0x"));
    CHECK(result.contains("_"));

    // Each member appears as contiguous hex (byte order depends on endianness)
    // Members may be separated by _ but won't be split internally
    CHECK((result.contains("12345678") || result.contains("78563412")));
    CHECK((result.contains("ABCD") || result.contains("CDAB")));
    CHECK(result.contains("EF"));
}

TEST("to_debug_string - memory dump has alignment separators")
{
    struct Aligned16
    {
        alignas(16) uint64_t data[4];
    };

    Aligned16 obj{{0, 0, 0, 0}};
    auto result = cc::to_debug_string(obj);

    // Format is raw(0x[HEX]_[HEX]...)
    CHECK(result.contains("0x"));
    CHECK(result.contains("_"));

    // 32 bytes of zeros - should contain hex representation
    CHECK(result.size() > 10);
}

// =========================================================================================================
// max_length truncation tests
// =========================================================================================================

TEST("to_debug_string - large iterable truncates with ellipsis")
{
    std::vector<int> large;
    for (int i = 0; i < 1000; ++i)
        large.push_back(i);

    cc::debug_string_config cfg{100};
    auto result = cc::to_debug_string(large, cfg);

    // Should be truncated
    CHECK(result.ends_with(", ...]"));

    // Should be bounded (not strict, allow some overshoot)
    CHECK(result.size() < 200);
}

TEST("to_debug_string - large iterable respects max_length")
{
    std::vector<cc::string> verbose;
    for (int i = 0; i < 500; ++i)
        verbose.push_back("item_" + cc::to_string(i));

    cc::debug_string_config cfg{80};
    auto result = cc::to_debug_string(verbose, cfg);

    CHECK(result.ends_with(", ...]"));
    CHECK(result.size() < 150);
}

TEST("to_debug_string - large tuple truncates")
{
    auto large = std::make_tuple(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                                 25, 26, 27, 28, 29, 30);

    cc::debug_string_config cfg{50};
    auto result = cc::to_debug_string(large, cfg);

    CHECK(result.ends_with(", ...)"));
    CHECK(result.size() < 100);
}

TEST("to_debug_string - verbose tuple elements truncate")
{
    std::tuple<cc::string, cc::string, cc::string> verbose{"very_long_string_element_one_that_goes_on_and_on",
                                                           "another_extremely_long_string_element_two",
                                                           "yet_another_verbose_string_element_three"};

    cc::debug_string_config cfg{40};
    auto result = cc::to_debug_string(verbose, cfg);

    CHECK(result.ends_with(", ...)"));
    CHECK(result.size() < 100);
}

// =========================================================================================================
// Recursive truncation tests
// =========================================================================================================

TEST("to_debug_string - nested structure truncates recursively")
{
    std::vector<std::vector<int>> nested;
    for (int i = 0; i < 50; ++i)
    {
        std::vector<int> inner;
        for (int j = 0; j < 50; ++j)
            inner.push_back(i * 100 + j);
        nested.push_back(inner);
    }

    cc::debug_string_config cfg{100};
    auto result = cc::to_debug_string(nested, cfg);

    // Should truncate before processing all elements
    CHECK(result.ends_with(", ...]"));
    CHECK(result.size() < 250);
}

TEST("to_debug_string - deeply nested truncates at outer level")
{
    std::vector<std::vector<std::vector<int>>> deep;
    for (int i = 0; i < 20; ++i)
    {
        std::vector<std::vector<int>> mid;
        for (int j = 0; j < 20; ++j)
        {
            std::vector<int> inner;
            for (int k = 0; k < 20; ++k)
                inner.push_back(k);
            mid.push_back(inner);
        }
        deep.push_back(mid);
    }

    cc::debug_string_config cfg{120};
    auto result = cc::to_debug_string(deep, cfg);

    CHECK(result.ends_with(", ...]"));
    CHECK(result.size() < 300);
}

// =========================================================================================================
// Mixed dispatch in nested structures
// =========================================================================================================

TEST("to_debug_string - vector of tuples with mixed types")
{
    std::vector<std::tuple<int, cc::string>> mixed;
    mixed.push_back({1, "first"});
    mixed.push_back({2, "second"});
    mixed.push_back({3, "third"});

    auto result = cc::to_debug_string(mixed);

    // Strings are now wrapped in quotes
    CHECK(result == "[(1, \"first\"), (2, \"second\"), (3, \"third\")]");
}

TEST("to_debug_string - tuple containing vector and custom type")
{
    std::tuple<std::vector<int>, CustomStringable, cc::string> complex{{10, 20, 30}, CustomStringable{99}, "end"};

    auto result = cc::to_debug_string(complex);

    // String is now wrapped in quotes
    CHECK(result == "([10, 20, 30], 99_custom, \"end\")");
}

TEST("to_debug_string - vector of pairs")
{
    std::vector<std::pair<int, cc::string>> pairs;
    pairs.push_back({1, "a"});
    pairs.push_back({2, "b"});

    auto result = cc::to_debug_string(pairs);

    // Strings are now wrapped in quotes
    CHECK(result == "[(1, \"a\"), (2, \"b\")]");
}

TEST("to_debug_string - complex nested heterogeneous structure")
{
    std::vector<std::tuple<int, std::vector<cc::string>, CustomStringable>> complex;
    complex.push_back({1, {"x", "y"}, CustomStringable{5}});
    complex.push_back({2, {"z"}, CustomStringable{10}});

    auto result = cc::to_debug_string(complex);

    // Strings are now wrapped in quotes
    CHECK(result == "[(1, [\"x\", \"y\"], 5_custom), (2, [\"z\"], 10_custom)]");
}

// =========================================================================================================
// Edge cases: empty strings and zero-size types
// =========================================================================================================

TEST("to_debug_string - empty string via to_string")
{
    cc::string empty = "";
    auto result = cc::to_debug_string(empty);

    // Empty strings are wrapped in quotes to ensure non-empty output
    CHECK(result == "\"\"");
}

TEST("to_debug_string - empty struct")
{
    EmptyStruct empty;
    auto result = cc::to_debug_string(empty);

    // Empty structs are 1 byte in C++ (to ensure distinct addresses)
    // Should produce "0x" followed by 2 hex digits representing the single byte
    CHECK(result.starts_with("raw(0x"));
    CHECK(result.size() == 9); // "raw(0x??)"
}

TEST("to_debug_string - vector of empty strings")
{
    std::vector<cc::string> empties = {"", "", ""};
    auto result = cc::to_debug_string(empties);

    // Empty strings now show as "" to make them visible
    CHECK(result == "[\"\", \"\", \"\"]");
}

TEST("to_debug_string - tuple with empty string")
{
    std::tuple<int, cc::string, int> with_empty{1, "", 2};
    auto result = cc::to_debug_string(with_empty);

    // Empty string now shows as "" to make it visible
    CHECK(result == "(1, \"\", 2)");
}

// =========================================================================================================
// Additional basic type coverage
// =========================================================================================================

TEST("to_debug_string - primitive types via to_string")
{
    SECTION("int")
    {
        CHECK(cc::to_debug_string(42) == "42");
        CHECK(cc::to_debug_string(-17) == "-17");
    }

    SECTION("bool")
    {
        CHECK(cc::to_debug_string(true) == "true");
        CHECK(cc::to_debug_string(false) == "false");
    }

    SECTION("float")
    {
        auto result = cc::to_debug_string(3.14f);
        // Just verify it produces something reasonable
        CHECK(result.size() > 0);
    }
}

TEST("to_debug_string - default config uses max_length 100")
{
    std::vector<int> large;
    for (int i = 0; i < 1000; ++i)
        large.push_back(i);

    // Use default config
    auto result = cc::to_debug_string(large);

    // Should be truncated with default limit
    CHECK(result.ends_with(", ...]"));
}

// =========================================================================================================
// String wrapping tests (ensures non-empty output)
// =========================================================================================================

TEST("to_debug_string - string wrapping ensures non-empty output")
{
    SECTION("empty string")
    {
        cc::string empty = "";
        CHECK(cc::to_debug_string(empty) == "\"\"");
    }

    SECTION("single space")
    {
        cc::string space = " ";
        CHECK(cc::to_debug_string(space) == "\" \"");
    }

    SECTION("normal string")
    {
        cc::string normal = "test";
        CHECK(cc::to_debug_string(normal) == "\"test\"");
    }

    SECTION("string with quotes")
    {
        cc::string with_quotes = "hello \"world\"";
        // Just verify it's wrapped; internal quote handling may vary
        auto result = cc::to_debug_string(with_quotes);
        CHECK(result.starts_with("\""));
        CHECK(result.ends_with("\""));
    }
}

// =========================================================================================================
// Char escaping tests (ensures special chars are visible)
// =========================================================================================================

TEST("to_debug_string - char printables show as-is")
{
    SECTION("lowercase letter")
    {
        CHECK(cc::to_debug_string('a') == "'a'");
    }

    SECTION("uppercase letter")
    {
        CHECK(cc::to_debug_string('Z') == "'Z'");
    }

    SECTION("digit")
    {
        CHECK(cc::to_debug_string('5') == "'5'");
    }

    SECTION("space")
    {
        CHECK(cc::to_debug_string(' ') == "' '");
    }

    SECTION("punctuation")
    {
        CHECK(cc::to_debug_string('!') == "'!'");
        CHECK(cc::to_debug_string(',') == "','");
    }
}

TEST("to_debug_string - char common escapes")
{
    SECTION("newline")
    {
        CHECK(cc::to_debug_string('\n') == "'\\n'");
    }

    SECTION("tab")
    {
        CHECK(cc::to_debug_string('\t') == "'\\t'");
    }

    SECTION("carriage return")
    {
        CHECK(cc::to_debug_string('\r') == "'\\r'");
    }

    SECTION("null terminator")
    {
        CHECK(cc::to_debug_string('\0') == "'\\0'");
    }

    SECTION("backslash")
    {
        CHECK(cc::to_debug_string('\\') == "'\\\\'");
    }

    SECTION("single quote")
    {
        CHECK(cc::to_debug_string('\'') == "'\\''");
    }

    SECTION("vertical tab")
    {
        CHECK(cc::to_debug_string('\v') == "'\\v'");
    }

    SECTION("form feed")
    {
        CHECK(cc::to_debug_string('\f') == "'\\f'");
    }

    SECTION("backspace")
    {
        CHECK(cc::to_debug_string('\b') == "'\\b'");
    }

    SECTION("alert/bell")
    {
        CHECK(cc::to_debug_string('\a') == "'\\a'");
    }
}

TEST("to_debug_string - char control characters as hex")
{
    SECTION("ASCII 1 (SOH)")
    {
        CHECK(cc::to_debug_string('\x01') == "'\\x01'");
    }

    SECTION("ASCII 2 (STX)")
    {
        CHECK(cc::to_debug_string('\x02') == "'\\x02'");
    }

    SECTION("ASCII 27 (ESC)")
    {
        CHECK(cc::to_debug_string('\x1B') == "'\\x1B'");
    }

    SECTION("ASCII 127 (DEL)")
    {
        CHECK(cc::to_debug_string('\x7F') == "'\\x7F'");
    }

    SECTION("ASCII 31 (US)")
    {
        CHECK(cc::to_debug_string('\x1F') == "'\\x1F'");
    }
}

TEST("to_debug_string - char ensures visibility in collections")
{
    SECTION("list of chars with escapes")
    {
        std::list<char> chars = {'a', '\n', 'b', '\t', 'c'};
        auto result = cc::to_debug_string(chars);
        CHECK(result == "['a', '\\n', 'b', '\\t', 'c']");
    }

    SECTION("list with null char")
    {
        std::list<char> with_null = {'x', '\0', 'y'};
        auto result = cc::to_debug_string(with_null);
        CHECK(result == "['x', '\\0', 'y']");
    }

    SECTION("tuple with mixed chars")
    {
        std::tuple<char, char, char> mixed{' ', '\n', 'A'};
        auto result = cc::to_debug_string(mixed);
        CHECK(result == "(' ', '\\n', 'A')");
    }
}

// =========================================================================================================
// Non-null terminated char const* tests
// =========================================================================================================

TEST("to_debug_string - non-null terminated char const* does not crash")
{
    // Create a non-null terminated char buffer
    const std::vector<char> v = {'h', 'i'};

    // This should not crash even though the buffer is not null-terminated
    auto result = cc::to_debug_string(v.data());

    // We just verify that it doesn't crash and produces some output
    // The exact output format isn't critical, but it should be safe
    CHECK(result.size() > 0);
}

// =========================================================================================================
// Nullptr tests
// =========================================================================================================

TEST("to_debug_string - nullptr produces <nullptr>")
{
    int* ptr = nullptr;
    auto result = cc::to_debug_string(ptr);
    CHECK(result == "<nullptr>");
}

TEST("to_debug_string - nullptr for different pointer types")
{
    SECTION("char pointer")
    {
        char* ptr = nullptr;
        CHECK(cc::to_debug_string(ptr) == "<nullptr>");
    }

    SECTION("void pointer")
    {
        void* ptr = nullptr;
        CHECK(cc::to_debug_string(ptr) == "<nullptr>");
    }

    SECTION("struct pointer")
    {
        OpaqueType* ptr = nullptr;
        CHECK(cc::to_debug_string(ptr) == "<nullptr>");
    }

    SECTION("custom type pointer")
    {
        HasOnlyMember* ptr = nullptr;
        CHECK(cc::to_debug_string(ptr) == "<nullptr>");
    }
}

TEST("to_debug_string - nullptr in containers")
{
    SECTION("vector of pointers with nullptr")
    {
        std::vector<int*> ptrs = {nullptr, nullptr};
        auto result = cc::to_debug_string(ptrs);
        CHECK(result == "[<nullptr>, <nullptr>]");
    }

    SECTION("tuple with nullptr")
    {
        std::tuple<int, char*, int> with_nullptr{42, nullptr, 99};
        auto result = cc::to_debug_string(with_nullptr);
        CHECK(result == "(42, <nullptr>, 99)");
    }
}

// =========================================================================================================
// Optional tests
// =========================================================================================================

TEST("to_debug_string - optional without value shows nullopt")
{
    cc::optional<int> opt;
    auto result = cc::to_debug_string(opt);
    CHECK(result == "nullopt");
}

TEST("to_debug_string - optional with value shows value(...)")
{
    cc::optional<int> opt = 42;
    auto result = cc::to_debug_string(opt);
    CHECK(result == "value(42)");
}

TEST("to_debug_string - optional with different types")
{
    SECTION("optional<bool>")
    {
        cc::optional<bool> opt_empty;
        cc::optional<bool> opt_true = true;
        cc::optional<bool> opt_false = false;

        CHECK(cc::to_debug_string(opt_empty) == "nullopt");
        CHECK(cc::to_debug_string(opt_true) == "value(true)");
        CHECK(cc::to_debug_string(opt_false) == "value(false)");
    }

    SECTION("optional<string>")
    {
        cc::optional<cc::string> opt_empty;
        cc::optional<cc::string> opt_value = "hello";

        CHECK(cc::to_debug_string(opt_empty) == "nullopt");
        CHECK(cc::to_debug_string(opt_value) == "value(\"hello\")");
    }

    SECTION("optional<vector>")
    {
        cc::optional<std::vector<int>> opt_empty;
        cc::optional<std::vector<int>> opt_value = std::vector<int>{1, 2, 3};

        CHECK(cc::to_debug_string(opt_empty) == "nullopt");
        CHECK(cc::to_debug_string(opt_value) == "value([1, 2, 3])");
    }

    SECTION("optional<char>")
    {
        cc::optional<char> opt_empty;
        cc::optional<char> opt_newline = '\n';
        cc::optional<char> opt_char = 'x';

        CHECK(cc::to_debug_string(opt_empty) == "nullopt");
        CHECK(cc::to_debug_string(opt_newline) == "value('\\n')");
        CHECK(cc::to_debug_string(opt_char) == "value('x')");
    }
}

TEST("to_debug_string - optional in containers")
{
    SECTION("vector of optionals")
    {
        std::vector<cc::optional<int>> opts;
        opts.push_back(cc::nullopt);
        opts.push_back(10);
        opts.push_back(cc::nullopt);
        opts.push_back(20);

        auto result = cc::to_debug_string(opts);
        CHECK(result == "[nullopt, value(10), nullopt, value(20)]");
    }

    SECTION("tuple with optionals")
    {
        std::tuple<cc::optional<int>, int, cc::optional<cc::string>> mixed{42, 99, cc::nullopt};
        auto result = cc::to_debug_string(mixed);
        CHECK(result == "(value(42), 99, nullopt)");
    }

    SECTION("nested optional")
    {
        cc::optional<cc::optional<int>> nested_empty;
        cc::optional<cc::optional<int>> nested_outer_only = cc::optional<int>{};
        cc::optional<cc::optional<int>> nested_full = cc::optional<int>{42};

        CHECK(cc::to_debug_string(nested_empty) == "nullopt");
        CHECK(cc::to_debug_string(nested_outer_only) == "value(nullopt)");
        CHECK(cc::to_debug_string(nested_full) == "value(value(42))");
    }
}

TEST("to_debug_string - optional with custom types")
{
    SECTION("optional with custom stringable")
    {
        cc::optional<CustomStringable> opt_empty;
        cc::optional<CustomStringable> opt_value = CustomStringable{99};

        CHECK(cc::to_debug_string(opt_empty) == "nullopt");
        CHECK(cc::to_debug_string(opt_value) == "value(99_custom)");
    }

    SECTION("optional with tuple")
    {
        cc::optional<std::tuple<int, int>> opt_empty;
        cc::optional<std::tuple<int, int>> opt_value = std::make_tuple(10, 20);

        CHECK(cc::to_debug_string(opt_empty) == "nullopt");
        CHECK(cc::to_debug_string(opt_value) == "value((10, 20))");
    }
}
