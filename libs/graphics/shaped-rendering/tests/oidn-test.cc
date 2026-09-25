#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-rendering/impl/oidn_device.hh>

// The OIDN dependency, and whether this build can actually reach it.
//
// Three things can be true independently, and only the first is a compile-time fact: the library was linked, its
// runtime sits beside this binary, and a device comes up on this machine.
// OIDN's facade loads its core and its CPU device module BY NAME out of its own directory, so a build that linked it
// and staged only the import library's DLL runs and then fails at the first filter — which is a failure worth having
// a test for rather than discovering in a denoised image that never arrives.

TEST("sr - the OIDN dependency links and reports its version")
{
    if (!sr::impl::oidn_is_compiled_in())
    {
        CHECK(sr::impl::oidn_version().empty());
        CHECK(!sr::impl::oidn_has_device());
        return;
    }

    // Major version rather than the exact string: the buffer contract and the filter names are what move with it,
    // and pinning the patch here would make every bump a test edit.
    auto const version = sr::impl::oidn_version();
    CHECK(version.starts_with("2.")).context(cc::format("OIDN reports version '{}'", version));
}

// The CPU device, which is the one the member runs on and the one that needs no vendor hardware.
//
// Skipped rather than failed where OIDN was not fetched, because that is a legitimate build; where it WAS fetched,
// a device that does not come up is a broken install rather than an absent one.
TEST("sr - an OIDN CPU device comes up")
{
    if (!sr::impl::oidn_is_compiled_in())
        SKIP("OIDN was not fetched into this build (extern/oidn/fetch-oidn.py)");

    CHECK(sr::impl::oidn_has_device())
        .context("OIDN linked but no CPU device — its core and device modules have to sit beside this binary");
}
