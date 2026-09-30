#include "oidn_reference.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <nexus/test.hh>

// The OIDN dependency, and whether this build can actually reach it.
//
// Three things can be true independently, and only the first is a compile-time fact: the library was linked, its
// runtime sits beside this binary, and a device comes up on this machine.
// OIDN's facade loads its core and its CPU device module BY NAME out of its own directory, so a build that linked it
// and staged only the import library's DLL runs and then fails at the first filter — which is a failure worth having
// a test for rather than discovering in a denoised image that never arrives.

TEST("sr - the OIDN dependency links and reports its version")
{
    if (!sr_test::oidn_is_compiled_in())
    {
        CHECK(sr_test::oidn_version().empty());
        CHECK(!sr_test::oidn_has_device());
        return;
    }

    // Major version rather than the exact string: the buffer contract and the filter names are what move with it,
    // and pinning the patch here would make every bump a test edit.
    auto const version = sr_test::oidn_version();
    CHECK(version.starts_with("2.")).context(cc::format("OIDN reports version '{}'", version));
}

// The CPU device, which is the one the oracle filters on and the one that needs no vendor hardware.
//
// Skipped rather than failed where OIDN was not fetched, because that is a legitimate build; where it WAS fetched,
// a device that does not come up is a broken install rather than an absent one.
// How long Intel's CPU device takes over a 1080p frame, the number set beside the member's in
// libs/graphics/shaped-rendering/docs/reconstruction.md.
//
// Manual because it is a measurement rather than a check, and it takes seconds.
TEST("sr - OIDN's CPU device timed at 1080p", nx::config::manual)
{
    if (!sr_test::oidn_is_compiled_in())
        SKIP("OIDN was not fetched into this build (uv run extern/oidn/fetch-oidn.py)");

    for (auto const size : {sr::oidn_network_size::base, sr::oidn_network_size::small})
    {
        auto const secs = sr_test::oidn_time_cpu_filter(tg::vec2i(1920, 1080), size, 5);
        REQUIRE(secs > 0.0);
        cc::println("OIDN CPU, {} network, 1920x1080: {} ms (best of 5)",
                    size == sr::oidn_network_size::small ? "small" : "base", secs * 1000.0);
    }
}

TEST("sr - an OIDN CPU device comes up")
{
    if (!sr_test::oidn_is_compiled_in())
        SKIP("OIDN was not fetched into this build (uv run extern/oidn/fetch-oidn.py)");

    CHECK(sr_test::oidn_has_device())
        .context("OIDN linked but no CPU device — its core and device modules have to sit beside this binary");
}
