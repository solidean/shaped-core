#include <nexus/test.hh>
#include <shaped-graphics/context/cold_caches.hh>

// The parser alone: setting SC_SG_COLD in a test would race every other test's pipeline build reading it.

TEST("sg cold caches - unset or empty is all warm")
{
    auto const c = sg::parse_cold_caches("");
    CHECK(!c.pipelines);
    CHECK(!c.shaders);
}

TEST("sg cold caches - each tier is named on its own, and all names both")
{
    CHECK(sg::parse_cold_caches("pipelines").pipelines);
    CHECK(!sg::parse_cold_caches("pipelines").shaders);
    CHECK(sg::parse_cold_caches("shaders").shaders);
    CHECK(!sg::parse_cold_caches("shaders").pipelines);

    auto const all = sg::parse_cold_caches("all");
    CHECK(all.pipelines);
    CHECK(all.shaders);
}

TEST("sg cold caches - tiers combine with commas")
{
    auto const both = sg::parse_cold_caches("shaders,pipelines");
    CHECK(both.pipelines);
    CHECK(both.shaders);
}

TEST("sg cold caches - an unknown name warns and leaves the tiers warm")
{
    nx::expect_warning("none of 'pipelines', 'shaders' or 'all'");
    auto const c = sg::parse_cold_caches("everything");
    CHECK(!c.pipelines);
    CHECK(!c.shaders);
}
