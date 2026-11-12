#include <nexus/test.hh>
#include <shaped-graphics/context/gpu_selector.hh>

// The matcher alone: setting SC_GPU here would move every other test's context to another GPU.

TEST("sg gpu selector - an empty selector matches every adapter")
{
    CHECK(sg::gpu_selector_matches("", 0x8086, "Intel(R) Arc(TM) B580 Graphics"));
    CHECK(sg::gpu_selector_matches("", 0, ""));
}

TEST("sg gpu selector - a vendor name matches by PCI id, whatever the adapter's name says")
{
    CHECK(sg::gpu_selector_matches("intel", 0x8086, "Intel(R) Arc(TM) B580 Graphics"));
    CHECK(sg::gpu_selector_matches("AMD", 0x1002, "AMD Radeon(TM) Graphics"));
    CHECK(sg::gpu_selector_matches("nvidia", 0x10DE, "NVIDIA GeForce RTX 4090"));

    // The name alone does not make the vendor: a vendor selector never falls back to a substring match.
    CHECK(!sg::gpu_selector_matches("amd", 0x8086, "Intel AMD-compatible test adapter"));
    CHECK(!sg::gpu_selector_matches("intel", 0x1002, "AMD Radeon(TM) Graphics"));
}

TEST("sg gpu selector - anything else is a case-insensitive substring of the name")
{
    CHECK(sg::gpu_selector_matches("B580", 0x8086, "Intel(R) Arc(TM) B580 Graphics"));
    CHECK(sg::gpu_selector_matches("radeon", 0x1002, "AMD Radeon(TM) Graphics"));
    CHECK(sg::gpu_selector_matches("arc(tm) b5", 0x8086, "Intel(R) Arc(TM) B580 Graphics"));
    CHECK(!sg::gpu_selector_matches("B570", 0x8086, "Intel(R) Arc(TM) B580 Graphics"));
    CHECK(!sg::gpu_selector_matches("Graphics and more", 0x8086, "Graphics"));
}
