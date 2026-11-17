#include <nexus/test.hh>
#include <shaped-graphics/context/native_debug_info.hh>

// The parser alone: setting SC_SG_NATIVE_DEBUG_INFO in a test would race every other test's context creation reading it.

TEST("sg native debug info - each level is named by its own word")
{
    CHECK(sg::parse_native_debug_info("none") == sg::native_debug_info::none);
    CHECK(sg::parse_native_debug_info("lines") == sg::native_debug_info::lines);
    CHECK(sg::parse_native_debug_info("full") == sg::native_debug_info::full);
}

TEST("sg native debug info - a word that names no level is nothing")
{
    CHECK(!sg::parse_native_debug_info("").has_value());
    CHECK(!sg::parse_native_debug_info("Full").has_value());
    CHECK(!sg::parse_native_debug_info("true").has_value());
}
