#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>

// Path normalization is slib's traversal guard, not a tidiness pass: a mount can only reach outside
// itself if normalize_path lets a '..' through. These pin that directly.

using slib::impl::is_path_under;
using slib::impl::join_path;
using slib::impl::normalize_path;
using slib::impl::parent_path;
using slib::impl::relative_to;

namespace
{
// normalize_path -> the string, or "<escaped>" for the reject case, so a CHECK reads as one comparison.
cc::string normalized_or_escaped(cc::string_view path)
{
    auto const r = normalize_path(path);
    return r.has_value() ? r.value() : cc::string("<escaped>");
}
} // namespace

TEST("slib - normalize_path collapses noise")
{
    CHECK(normalized_or_escaped("a/b.hlsl") == "a/b.hlsl");
    CHECK(normalized_or_escaped("") == "");

    SECTION("separators")
    {
        CHECK(normalized_or_escaped("a//b") == "a/b");
        CHECK(normalized_or_escaped("/a/b") == "a/b"); // result is root-relative
        CHECK(normalized_or_escaped("a/b/") == "a/b"); // trailing separator drops
        CHECK(normalized_or_escaped("a\\b") == "a/b"); // native separators are accepted
        CHECK(normalized_or_escaped("///") == "");
    }

    SECTION("dot segments")
    {
        CHECK(normalized_or_escaped("./a") == "a");
        CHECK(normalized_or_escaped("a/./b") == "a/b");
        CHECK(normalized_or_escaped("a/b/../c") == "a/c");
        CHECK(normalized_or_escaped("a/b/..") == "a");
        CHECK(normalized_or_escaped("a/../b") == "b");
    }
}

TEST("slib - normalize_path rejects paths escaping the root")
{
    CHECK(normalized_or_escaped("..") == "<escaped>");
    CHECK(normalized_or_escaped("../a") == "<escaped>");
    CHECK(normalized_or_escaped("a/../..") == "<escaped>");
    CHECK(normalized_or_escaped("a/../../b") == "<escaped>");
    CHECK(normalized_or_escaped("/../a") == "<escaped>");
    CHECK(normalized_or_escaped("..\\a") == "<escaped>");

    // Popping back to the root is fine — it is only going *past* it that escapes.
    CHECK(normalized_or_escaped("a/..") == "");
}

TEST("slib - join_path")
{
    CHECK(join_path("a", "b.hlsl").value() == "a/b.hlsl");
    CHECK(join_path("", "b.hlsl").value() == "b.hlsl");
    CHECK(join_path("a/b", "../c.hlsl").value() == "a/c.hlsl"); // an include reaching a sibling folder
    CHECK(!join_path("a", "../../b").has_value());              // still cannot escape
}

TEST("slib - parent_path")
{
    CHECK(parent_path("a/b/c.hlsl") == "a/b");
    CHECK(parent_path("c.hlsl") == "");
    CHECK(parent_path("") == "");
}

TEST("slib - is_path_under respects segment boundaries")
{
    CHECK(is_path_under("a/b.hlsl", "a"));
    CHECK(is_path_under("a", "a"));          // the prefix itself
    CHECK(is_path_under("anything", ""));    // the empty prefix is the root
    CHECK(!is_path_under("ab/c.hlsl", "a")); // "ab" is not under "a" despite the string prefix
    CHECK(!is_path_under("b/c.hlsl", "a"));
}

TEST("slib - relative_to strips the prefix")
{
    CHECK(relative_to("a/b/c.hlsl", "a") == "b/c.hlsl");
    CHECK(relative_to("a/b/c.hlsl", "a/b") == "c.hlsl");
    CHECK(relative_to("a/b/c.hlsl", "") == "a/b/c.hlsl");
    CHECK(relative_to("a", "a") == "");
    CHECK_ASSERTS(relative_to("b/c.hlsl", "a"));
}
