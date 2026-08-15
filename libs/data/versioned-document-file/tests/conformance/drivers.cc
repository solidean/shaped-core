#include "store_fixture.hh"

#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/invoke_tests.hh>

/// The drivers: one per store implementation, each running the WHOLE conformance suite.
///
/// A test that only one arm could pass would not be a conformance test, so there is no per-arm test file — the two
/// entries below are the only place the arms are named.

TEST("vdoc::file - the in-memory store")
{
    nx::invoke_tests("in-memory", vdoc::file::test::in_memory_impl());
}

TEST("vdoc::file - the sqlite store")
{
    auto const impl = vdoc::file::test::sqlite_impl();
    if (!impl.is_available())
        SKIP("the SQLite backend was not compiled in");
    else
        nx::invoke_tests("sqlite", impl);
}

/// Defines, per conformance test, an alias that runs it on both arms.
/// So `dev.py test "<name>"` selects one behaviour and checks it everywhere it has to hold.
NX_TEST_SETUP(nx::setup& s)
{
    auto const drivers = {cc::pair<cc::string_view, cc::string_view>("vdoc::file - the in-memory store", "in-memory"),
                          cc::pair<cc::string_view, cc::string_view>("vdoc::file - the sqlite store", "sqlite")};

    for (auto const* invocable : s.invocables_with<vdoc::file::test::store_impl>())
    {
        auto fragments = cc::vector<nx::alias_fragment>();
        for (auto const& [test_name, section] : drivers)
            if (auto const* driver = s.find_test(test_name))
                fragments.push_back({.driver = driver, .section_path = {section, invocable->name}});

        if (!fragments.empty())
            s.define_alias(cc::string(invocable->name), cc::move(fragments));
    }
}
