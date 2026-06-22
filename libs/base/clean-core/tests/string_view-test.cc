#include <clean-core/char_predicates.hh>
#include <clean-core/string.hh>
#include <clean-core/string_view.hh>
#include <clean-core/utility.hh>

#include <nexus/test.hh>

#include <string>
#include <string_view>
#include <vector>

// static assertions for triviality
static_assert(std::is_trivially_copyable_v<cc::string_view>, "string_view should be trivially copyable");
static_assert(std::is_trivially_destructible_v<cc::string_view>, "string_view should be trivially destructible");

TEST("string_view - construction")
{
    SECTION("default construction")
    {
        auto const sv = cc::string_view{};
        CHECK(sv.data() == nullptr);
        CHECK(sv.size() == 0);
        CHECK(sv.empty());
    }

    SECTION("nullptr construction deleted")
    {
        // cc::string_view sv = nullptr; // should not compile
        SUCCEED();
    }

    SECTION("C string construction")
    {
        char const* cstr = "hello";
        auto const sv = cc::string_view{cstr};
        CHECK(sv.data() == cstr);
        CHECK(sv.size() == 5);
        CHECK(!sv.empty());
    }

    SECTION("pointer + size construction")
    {
        char const* str = "hello world";
        auto const sv = cc::string_view{str, 5};
        CHECK(sv.data() == str);
        CHECK(sv.size() == 5);
        CHECK(sv[0] == 'h');
        CHECK(sv[4] == 'o');
    }

    SECTION("two pointer construction")
    {
        char const* str = "hello world";
        auto const sv = cc::string_view{str, str + 5};
        CHECK(sv.data() == str);
        CHECK(sv.size() == 5);
    }

    SECTION("string literal construction")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.size() == 5);
        CHECK(sv[0] == 'h');
        CHECK(sv[4] == 'o');
    }

    SECTION("empty string literal")
    {
        auto const sv = cc::string_view{""};
        CHECK(sv.size() == 0);
        CHECK(sv.empty());
    }

    SECTION("container construction - cc::string")
    {
        auto const str = cc::string{"hello world"};
        auto const sv = cc::string_view{str};
        CHECK(sv.data() == str.data());
        CHECK(sv.size() == 11);
        CHECK(sv[0] == 'h');
    }

    SECTION("container construction - std::string")
    {
        auto const str = std::string{"hello world"};
        auto const sv = cc::string_view{str};
        CHECK(sv.data() == str.data());
        CHECK(sv.size() == 11);
        CHECK(sv[0] == 'h');
        CHECK(sv[10] == 'd');
    }

    SECTION("container construction - std::string_view")
    {
        auto const std_sv = std::string_view{"hello world"};
        auto const sv = cc::string_view{std_sv};
        CHECK(sv.data() == std_sv.data());
        CHECK(sv.size() == 11);
        CHECK(sv[0] == 'h');
        CHECK(sv[10] == 'd');
    }

    SECTION("container construction - std::vector<char>")
    {
        auto const vec = std::vector<char>{'h', 'e', 'l', 'l', 'o'};
        auto const sv = cc::string_view{vec};
        CHECK(sv.data() == vec.data());
        CHECK(sv.size() == 5);
        CHECK(sv[0] == 'h');
        CHECK(sv[4] == 'o');
    }

    SECTION("container construction - empty std::string")
    {
        auto const str = std::string{};
        auto const sv = cc::string_view{str};
        CHECK(sv.size() == 0);
        CHECK(sv.empty());
    }

    SECTION("container construction - empty std::vector<char>")
    {
        auto const vec = std::vector<char>{};
        auto const sv = cc::string_view{vec};
        CHECK(sv.size() == 0);
        CHECK(sv.empty());
    }
}

TEST("string_view - element access")
{
    SECTION("operator[]")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv[0] == 'h');
        CHECK(sv[1] == 'e');
        CHECK(sv[2] == 'l');
        CHECK(sv[3] == 'l');
        CHECK(sv[4] == 'o');
    }

    SECTION("front")
    {
        auto const sv = cc::string_view{"test"};
        CHECK(sv.front() == 't');
    }

    SECTION("back")
    {
        auto const sv = cc::string_view{"test"};
        CHECK(sv.back() == 't');
    }

    SECTION("data")
    {
        char const* str = "hello";
        auto const sv = cc::string_view{str};
        CHECK(sv.data() == str);
    }

    SECTION("single character")
    {
        auto const sv = cc::string_view{"x"};
        CHECK(sv.front() == 'x');
        CHECK(sv.back() == 'x');
        CHECK(sv[0] == 'x');
    }
}

TEST("string_view - iterators")
{
    SECTION("begin/end")
    {
        char const* str = "hello";
        auto const sv = cc::string_view{str};
        CHECK(sv.begin() == str);
        CHECK(sv.end() == str + 5);
    }

    SECTION("range-based for loop")
    {
        auto const sv = cc::string_view{"abc"};
        cc::string result;
        for (auto c : sv)
        {
            result.push_back(c);
        }
        CHECK(cc::string_view{result} == sv);
    }

    SECTION("empty iteration")
    {
        auto const sv = cc::string_view{};
        int count = 0;
        for ([[maybe_unused]] auto c : sv)
        {
            ++count;
        }
        CHECK(count == 0);
    }
}

TEST("string_view - substring operations")
{
    SECTION("subview with offset and size")
    {
        auto const sv = cc::string_view{"hello world"};
        auto const sub = sv.subview(6, 5);
        CHECK(sub.size() == 5);
        CHECK(sub == cc::string_view{"world"});
    }

    SECTION("subview with offset to end")
    {
        auto const sv = cc::string_view{"hello world"};
        auto const sub = sv.subview(6);
        CHECK(sub.size() == 5);
        CHECK(sub == cc::string_view{"world"});
    }

    SECTION("subview at boundaries")
    {
        auto const sv = cc::string_view{"test"};
        CHECK(sv.subview(0, 4) == sv);
        CHECK(sv.subview(4, 0).empty());
        CHECK(sv.subview(0, 0).empty());
    }

    SECTION("subview_clamped - normal range")
    {
        auto const sv = cc::string_view{"hello"};
        auto const sub = sv.subview_clamped(1, 3);
        CHECK(sub == cc::string_view{"ell"});
    }

    SECTION("subview_clamped - out of bounds offset")
    {
        auto const sv = cc::string_view{"hello"};
        auto const sub = sv.subview_clamped(10, 5);
        CHECK(sub.empty());
    }

    SECTION("subview_clamped - size exceeds bounds")
    {
        auto const sv = cc::string_view{"hello"};
        auto const sub = sv.subview_clamped(3, 10);
        CHECK(sub == cc::string_view{"lo"});
    }

    SECTION("remove_prefix")
    {
        auto sv = cc::string_view{"hello world"};
        sv.remove_prefix(6);
        CHECK(sv == cc::string_view{"world"});
    }

    SECTION("remove_suffix")
    {
        auto sv = cc::string_view{"hello world"};
        sv.remove_suffix(6);
        CHECK(sv == cc::string_view{"hello"});
    }

    SECTION("remove_prefix entire string")
    {
        auto sv = cc::string_view{"test"};
        sv.remove_prefix(4);
        CHECK(sv.empty());
    }

    SECTION("remove_suffix entire string")
    {
        auto sv = cc::string_view{"test"};
        sv.remove_suffix(4);
        CHECK(sv.empty());
    }
}

TEST("string_view - comparison")
{
    SECTION("equality - same content")
    {
        auto const sv1 = cc::string_view{"hello"};
        auto const sv2 = cc::string_view{"hello"};
        CHECK(sv1 == sv2);
        CHECK(!(sv1 != sv2));
    }

    SECTION("equality - different content")
    {
        auto const sv1 = cc::string_view{"hello"};
        auto const sv2 = cc::string_view{"world"};
        CHECK(sv1 != sv2);
        CHECK(!(sv1 == sv2));
    }

    SECTION("equality - different lengths")
    {
        auto const sv1 = cc::string_view{"hello"};
        auto const sv2 = cc::string_view{"hello world"};
        CHECK(sv1 != sv2);
    }

    SECTION("equality - empty views")
    {
        auto const sv1 = cc::string_view{};
        auto const sv2 = cc::string_view{""};
        CHECK(sv1 == sv2);
    }

    SECTION("compare - lexicographic ordering")
    {
        auto const sv1 = cc::string_view{"abc"};
        auto const sv2 = cc::string_view{"abd"};
        CHECK(sv1.compare(sv2) < 0);
        CHECK(sv2.compare(sv1) > 0);
        CHECK(sv1.compare(sv1) == 0);
    }

    SECTION("compare - different lengths")
    {
        auto const sv1 = cc::string_view{"abc"};
        auto const sv2 = cc::string_view{"abcd"};
        CHECK(sv1.compare(sv2) < 0);
        CHECK(sv2.compare(sv1) > 0);
    }

    SECTION("relational operators")
    {
        auto const sv1 = cc::string_view{"abc"};
        auto const sv2 = cc::string_view{"abd"};
        auto const sv3 = cc::string_view{"abc"};
        CHECK(sv1 < sv2);
        CHECK(sv2 > sv1);
        CHECK(sv1 <= sv2);
        CHECK(sv1 <= sv3);
        CHECK(sv2 >= sv1);
        CHECK(sv1 >= sv3);
    }
}

TEST("string_view - starts_with / ends_with")
{
    SECTION("starts_with - string_view")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.starts_with(cc::string_view{"hello"}));
        CHECK(sv.starts_with(cc::string_view{""}));
        CHECK(!sv.starts_with(cc::string_view{"world"}));
        CHECK(!sv.starts_with(cc::string_view{"hello world!"}));
    }

    SECTION("starts_with - char")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.starts_with('h'));
        CHECK(!sv.starts_with('e'));
    }

    SECTION("starts_with - empty view")
    {
        auto const sv = cc::string_view{};
        CHECK(!sv.starts_with('x'));
        CHECK(sv.starts_with(cc::string_view{""}));
    }

    SECTION("ends_with - string_view")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.ends_with(cc::string_view{"world"}));
        CHECK(sv.ends_with(cc::string_view{""}));
        CHECK(!sv.ends_with(cc::string_view{"hello"}));
        CHECK(!sv.ends_with(cc::string_view{"!hello world"}));
    }

    SECTION("ends_with - char")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.ends_with('o'));
        CHECK(!sv.ends_with('h'));
    }

    SECTION("ends_with - empty view")
    {
        auto const sv = cc::string_view{};
        CHECK(!sv.ends_with('x'));
        CHECK(sv.ends_with(cc::string_view{""}));
    }
}

TEST("string_view - contains")
{
    SECTION("contains - string_view")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.contains(cc::string_view{"hello"}));
        CHECK(sv.contains(cc::string_view{"world"}));
        CHECK(sv.contains(cc::string_view{"lo wo"}));
        CHECK(sv.contains(cc::string_view{""}));
        CHECK(!sv.contains(cc::string_view{"xyz"}));
    }

    SECTION("contains - char")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.contains('h'));
        CHECK(sv.contains('e'));
        CHECK(sv.contains('o'));
        CHECK(!sv.contains('x'));
    }

    SECTION("contains - empty view")
    {
        auto const sv = cc::string_view{};
        CHECK(!sv.contains('x'));
        CHECK(sv.contains(cc::string_view{""}));
    }
}

TEST("string_view - find")
{
    SECTION("find - substring at beginning")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.find(cc::string_view{"hello"}) == 0);
    }

    SECTION("find - substring in middle")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.find(cc::string_view{"lo wo"}) == 3);
    }

    SECTION("find - substring at end")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.find(cc::string_view{"world"}) == 6);
    }

    SECTION("find - substring not found")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.find(cc::string_view{"xyz"}) == -1);
    }

    SECTION("find - empty substring")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.find(cc::string_view{""}) == 0);
        CHECK(sv.find(cc::string_view{""}, 3) == 3);
    }

    SECTION("find - with position")
    {
        auto const sv = cc::string_view{"hello hello"};
        CHECK(sv.find(cc::string_view{"hello"}, 0) == 0);
        CHECK(sv.find(cc::string_view{"hello"}, 1) == 6);
        CHECK(sv.find(cc::string_view{"hello"}, 7) == -1);
    }

    SECTION("find - char")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.find('h') == 0);
        CHECK(sv.find('e') == 1);
        CHECK(sv.find('l') == 2);
        CHECK(sv.find('o') == 4);
        CHECK(sv.find('x') == -1);
    }

    SECTION("find - char with position")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.find('l', 0) == 2);
        CHECK(sv.find('l', 3) == 3);
        CHECK(sv.find('l', 4) == -1);
    }
}

TEST("string_view - rfind")
{
    SECTION("rfind - substring at end")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.rfind(cc::string_view{"world"}) == 6);
    }

    SECTION("rfind - substring in middle")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.rfind(cc::string_view{"lo"}) == 3);
    }

    SECTION("rfind - substring at beginning")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.rfind(cc::string_view{"hello"}) == 0);
    }

    SECTION("rfind - substring not found")
    {
        auto const sv = cc::string_view{"hello world"};
        CHECK(sv.rfind(cc::string_view{"xyz"}) == -1);
    }

    SECTION("rfind - multiple occurrences")
    {
        auto const sv = cc::string_view{"hello hello"};
        CHECK(sv.rfind(cc::string_view{"hello"}) == 6);
        CHECK(sv.rfind(cc::string_view{"hello"}, 5) == 0);
    }

    SECTION("rfind - empty substring")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.rfind(cc::string_view{""}) == 5);
        CHECK(sv.rfind(cc::string_view{""}, 3) == 3);
    }

    SECTION("rfind - char")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.rfind('o') == 4);
        CHECK(sv.rfind('l') == 3);
        CHECK(sv.rfind('h') == 0);
        CHECK(sv.rfind('x') == -1);
    }

    SECTION("rfind - char with position")
    {
        auto const sv = cc::string_view{"hello"};
        CHECK(sv.rfind('l', -1) == 3);
        CHECK(sv.rfind('l', 3) == 3);
        CHECK(sv.rfind('l', 2) == 2);
        CHECK(sv.rfind('l', 1) == -1);
    }
}

TEST("string_view - copy and move")
{
    SECTION("copy construction")
    {
        auto const sv1 = cc::string_view{"hello"};
        auto const sv2 = sv1;
        CHECK(sv2.data() == sv1.data());
        CHECK(sv2.size() == sv1.size());
        CHECK(sv2 == sv1);
    }

    SECTION("copy assignment")
    {
        auto const sv1 = cc::string_view{"hello"};
        auto sv2 = cc::string_view{"world"};
        sv2 = sv1;
        CHECK(sv2 == sv1);
    }

    SECTION("move construction")
    {
        auto sv1 = cc::string_view{"hello"};
        auto const sv2 = cc::move(sv1);
        CHECK(sv2 == cc::string_view{"hello"});
    }

    SECTION("move assignment")
    {
        auto sv1 = cc::string_view{"hello"};
        auto sv2 = cc::string_view{"world"};
        sv2 = cc::move(sv1);
        CHECK(sv2 == cc::string_view{"hello"});
    }
}

TEST("string_view - matching prefix")
{
    SECTION("decompose_matching_prefix - basic")
    {
        auto const decomp = cc::string_view::decompose_matching_prefix("hello world", "hello there");
        CHECK(decomp.prefix_lhs == cc::string_view{"hello "});
        CHECK(decomp.prefix_rhs == cc::string_view{"hello "});
        CHECK(decomp.middle_lhs == cc::string_view{"world"});
        CHECK(decomp.middle_rhs == cc::string_view{"there"});
    }

    SECTION("decompose_matching_prefix - no match")
    {
        auto const decomp = cc::string_view::decompose_matching_prefix("abc", "xyz");
        CHECK(decomp.prefix_lhs.empty());
        CHECK(decomp.prefix_rhs.empty());
        CHECK(decomp.middle_lhs == cc::string_view{"abc"});
        CHECK(decomp.middle_rhs == cc::string_view{"xyz"});
    }

    SECTION("decompose_matching_prefix - complete match")
    {
        auto const decomp = cc::string_view::decompose_matching_prefix("test", "test");
        CHECK(decomp.prefix_lhs == cc::string_view{"test"});
        CHECK(decomp.prefix_rhs == cc::string_view{"test"});
        CHECK(decomp.middle_lhs.empty());
        CHECK(decomp.middle_rhs.empty());
    }

    SECTION("decompose_matching_prefix - different lengths")
    {
        auto const decomp = cc::string_view::decompose_matching_prefix("hello world", "hello");
        CHECK(decomp.prefix_lhs == cc::string_view{"hello"});
        CHECK(decomp.prefix_rhs == cc::string_view{"hello"});
        CHECK(decomp.middle_lhs == cc::string_view{" world"});
        CHECK(decomp.middle_rhs.empty());
    }

    SECTION("matching_prefix_of")
    {
        auto const prefix = cc::string_view::matching_prefix_of("hello world", "hello there");
        CHECK(prefix == cc::string_view{"hello "});
    }

    SECTION("strip_matching_prefix_of")
    {
        auto const stripped = cc::string_view::strip_matching_prefix_of("hello world", "hello there");
        CHECK(stripped == cc::string_view{"world"});
    }

    SECTION("matching_prefix_with")
    {
        auto const sv = cc::string_view{"hello world"};
        auto const prefix = sv.matching_prefix_with("hello there");
        CHECK(prefix == cc::string_view{"hello "});
    }

    SECTION("strip_matching_prefix_with - mutating")
    {
        auto sv = cc::string_view{"hello world"};
        sv.strip_matching_prefix_with("hello there");
        CHECK(sv == cc::string_view{"world"});
    }

    SECTION("stripped_matching_prefix_with - non-mutating")
    {
        auto const sv = cc::string_view{"hello world"};
        auto const stripped = sv.stripped_matching_prefix_with("hello there");
        CHECK(stripped == cc::string_view{"world"});
        CHECK(sv == cc::string_view{"hello world"});
    }
}

TEST("string_view - matching suffix")
{
    SECTION("decompose_matching_suffix - basic")
    {
        auto const decomp = cc::string_view::decompose_matching_suffix("prefix_test", "other_test");
        CHECK(decomp.middle_lhs == cc::string_view{"prefix"});
        CHECK(decomp.middle_rhs == cc::string_view{"other"});
        CHECK(decomp.suffix_lhs == cc::string_view{"_test"});
        CHECK(decomp.suffix_rhs == cc::string_view{"_test"});
    }

    SECTION("decompose_matching_suffix - no match")
    {
        auto const decomp = cc::string_view::decompose_matching_suffix("abc", "xyz");
        CHECK(decomp.middle_lhs == cc::string_view{"abc"});
        CHECK(decomp.middle_rhs == cc::string_view{"xyz"});
        CHECK(decomp.suffix_lhs.empty());
        CHECK(decomp.suffix_rhs.empty());
    }

    SECTION("decompose_matching_suffix - complete match")
    {
        auto const decomp = cc::string_view::decompose_matching_suffix("test", "test");
        CHECK(decomp.middle_lhs.empty());
        CHECK(decomp.middle_rhs.empty());
        CHECK(decomp.suffix_lhs == cc::string_view{"test"});
        CHECK(decomp.suffix_rhs == cc::string_view{"test"});
    }

    SECTION("matching_suffix_of")
    {
        auto const suffix = cc::string_view::matching_suffix_of("prefix_test", "other_test");
        CHECK(suffix == cc::string_view{"_test"});
    }

    SECTION("strip_matching_suffix_of")
    {
        auto const stripped = cc::string_view::strip_matching_suffix_of("prefix_test", "other_test");
        CHECK(stripped == cc::string_view{"prefix"});
    }

    SECTION("matching_suffix_with")
    {
        auto const sv = cc::string_view{"prefix_test"};
        auto const suffix = sv.matching_suffix_with("other_test");
        CHECK(suffix == cc::string_view{"_test"});
    }

    SECTION("strip_matching_suffix_with - mutating")
    {
        auto sv = cc::string_view{"prefix_test"};
        sv.strip_matching_suffix_with("other_test");
        CHECK(sv == cc::string_view{"prefix"});
    }

    SECTION("stripped_matching_suffix_with - non-mutating")
    {
        auto const sv = cc::string_view{"prefix_test"};
        auto const stripped = sv.stripped_matching_suffix_with("other_test");
        CHECK(stripped == cc::string_view{"prefix"});
        CHECK(sv == cc::string_view{"prefix_test"});
    }
}

TEST("string_view - matching affixes")
{
    SECTION("decompose_matching_affixes - basic")
    {
        auto const decomp = cc::string_view::decompose_matching_affixes("prefix_A_suffix", "prefix_B_suffix");
        CHECK(decomp.prefix_lhs == cc::string_view{"prefix_"});
        CHECK(decomp.prefix_rhs == cc::string_view{"prefix_"});
        CHECK(decomp.middle_lhs == cc::string_view{"A"});
        CHECK(decomp.middle_rhs == cc::string_view{"B"});
        CHECK(decomp.suffix_lhs == cc::string_view{"_suffix"});
        CHECK(decomp.suffix_rhs == cc::string_view{"_suffix"});
    }

    SECTION("decompose_matching_affixes - no match")
    {
        auto const decomp = cc::string_view::decompose_matching_affixes("abc", "xyz");
        CHECK(decomp.prefix_lhs.empty());
        CHECK(decomp.prefix_rhs.empty());
        CHECK(decomp.middle_lhs == cc::string_view{"abc"});
        CHECK(decomp.middle_rhs == cc::string_view{"xyz"});
        CHECK(decomp.suffix_lhs.empty());
        CHECK(decomp.suffix_rhs.empty());
    }

    SECTION("decompose_matching_affixes - prefix only")
    {
        auto const decomp = cc::string_view::decompose_matching_affixes("prefix_A", "prefix_B");
        CHECK(decomp.prefix_lhs == cc::string_view{"prefix_"});
        CHECK(decomp.middle_lhs == cc::string_view{"A"});
        CHECK(decomp.middle_rhs == cc::string_view{"B"});
        CHECK(decomp.suffix_lhs.empty());
    }

    SECTION("decompose_matching_affixes - suffix only")
    {
        auto const decomp = cc::string_view::decompose_matching_affixes("A_suffix", "B_suffix");
        CHECK(decomp.prefix_lhs.empty());
        CHECK(decomp.middle_lhs == cc::string_view{"A"});
        CHECK(decomp.middle_rhs == cc::string_view{"B"});
        CHECK(decomp.suffix_lhs == cc::string_view{"_suffix"});
    }

    SECTION("matching_affixes_of")
    {
        auto const [prefix, suffix] = cc::string_view::matching_affixes_of("pre_A_suf", "pre_B_suf");
        CHECK(prefix == cc::string_view{"pre_"});
        CHECK(suffix == cc::string_view{"_suf"});
    }

    SECTION("strip_matching_affixes_of")
    {
        auto const stripped = cc::string_view::strip_matching_affixes_of("prefix_A_suffix", "prefix_B_suffix");
        CHECK(stripped == cc::string_view{"A"});
    }

    SECTION("matching_affixes_with")
    {
        auto const sv = cc::string_view{"pre_A_suf"};
        auto const [prefix, suffix] = sv.matching_affixes_with("pre_B_suf");
        CHECK(prefix == cc::string_view{"pre_"});
        CHECK(suffix == cc::string_view{"_suf"});
    }

    SECTION("strip_matching_affixes_with - mutating")
    {
        auto sv = cc::string_view{"prefix_A_suffix"};
        sv.strip_matching_affixes_with("prefix_B_suffix");
        CHECK(sv == cc::string_view{"A"});
    }

    SECTION("stripped_matching_affixes_with - non-mutating")
    {
        auto const sv = cc::string_view{"prefix_A_suffix"};
        auto const stripped = sv.stripped_matching_affixes_with("prefix_B_suffix");
        CHECK(stripped == cc::string_view{"A"});
        CHECK(sv == cc::string_view{"prefix_A_suffix"});
    }
}

TEST("string_view - case-insensitive matching")
{
    SECTION("decompose_matching_prefix - case insensitive")
    {
        auto const decomp
            = cc::string_view::decompose_matching_prefix("Hello World", "HELLO There", cc::equal_case_insensitive{});
        CHECK(decomp.prefix_lhs == cc::string_view{"Hello "});
        CHECK(decomp.middle_lhs == cc::string_view{"World"});
        CHECK(decomp.middle_rhs == cc::string_view{"There"});
    }

    SECTION("decompose_matching_suffix - case insensitive")
    {
        auto const decomp
            = cc::string_view::decompose_matching_suffix("prefix_TEST", "other_test", cc::equal_case_insensitive{});
        CHECK(decomp.middle_lhs == cc::string_view{"prefix"});
        CHECK(decomp.middle_rhs == cc::string_view{"other"});
        CHECK(decomp.suffix_lhs == cc::string_view{"_TEST"});
    }

    SECTION("matching_prefix_with - case insensitive")
    {
        auto const sv = cc::string_view{"Hello World"};
        auto const prefix = sv.matching_prefix_with("hello there", cc::equal_case_insensitive{});
        CHECK(prefix == cc::string_view{"Hello "});
    }
}

TEST("string_view - special cases")
{
    SECTION("non-null-terminated substring")
    {
        char const* str = "hello world";
        auto const sv = cc::string_view{str, 5};
        CHECK(sv.size() == 5);
        CHECK(sv == cc::string_view{"hello"});
    }

    SECTION("comparison with different backing storage")
    {
        char const* str1 = "test";
        auto const str2 = cc::string{"test"};
        auto const sv1 = cc::string_view{str1};
        auto const sv2 = cc::string_view{str2};
        CHECK(sv1 == sv2);
        CHECK(sv1.data() != sv2.data());
    }

    SECTION("empty view at end of string")
    {
        char const* str = "hello";
        auto const sv = cc::string_view{str + 5, cc::isize(0)};
        CHECK(sv.empty());
        CHECK(sv.size() == 0);
    }
}
