#include <clean-core/platform/module_table.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// What a cc::source_location keeps at the build's SC_SOURCE_SITES level; every test holds at every level.

namespace
{
cc::source_location capture(cc::source_location s = cc::source_location::current())
{
    return s;
}
} // namespace

TEST("cc source_location - a default argument names the site that called")
{
    auto const a = capture();
    auto const b = capture();

    if constexpr (CC_SOURCE_SITES == 2)
    {
        CHECK(cc::string_view(a.file_name()).ends_with("source_location-test.cc"));
        CHECK(b.line() == a.line() + 1);
        CHECK(cc::site_address(a) == nullptr);
    }
    else
    {
        CHECK(cc::string_view(a.file_name()).empty());
        CHECK(cc::string_view(a.function_name()).empty());
        CHECK(a.line() == 0);
    }

    if constexpr (CC_SOURCE_SITES == 1)
    {
        // the address of the site's own code, so two sites differ, and both lie in this binary
        auto const addr_a = u64(reinterpret_cast<uintptr_t>(cc::site_address(a)));
        auto const addr_b = u64(reinterpret_cast<uintptr_t>(cc::site_address(b)));
        CHECK(addr_a != 0);
        CHECK(addr_a != addr_b);
        if (cc::module_enumeration_available())
        {
            auto in_a_module = false;
            for (auto const& m : cc::enumerate_loaded_modules())
                in_a_module = in_a_module || (m.contains(addr_a) && m.contains(addr_b));
            CHECK(in_a_module);
        }
    }

    if constexpr (CC_SOURCE_SITES == 0)
        CHECK(cc::site_address(a) == nullptr);
}

TEST("cc source_location - a constant-evaluated site is the level's empty one, or the full one")
{
    constexpr auto s = cc::source_location::current();
    if constexpr (CC_SOURCE_SITES == 2)
        CHECK(!cc::string_view(s.file_name()).empty());
    else
        CHECK(cc::site_address(s) == nullptr);
}

TEST("cc source_location - describe_site prints what the level kept")
{
    auto const text = cc::describe_site(capture());
    if constexpr (CC_SOURCE_SITES == 2)
        CHECK(text.contains("source_location-test.cc"));
    else if constexpr (CC_SOURCE_SITES == 1)
        CHECK(text.contains("+0x"));
    else
        CHECK(text.empty());
}

TEST("cc source_location - CC_SITE_TEXT keeps source text at FULL only")
{
    CHECK(cc::string_view(CC_SITE_TEXT("x < y")) == (CC_SOURCE_SITES == 2 ? "x < y" : ""));
}
