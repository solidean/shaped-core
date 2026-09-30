#include "sr_backends.hh"

#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/registry.hh>
#include <shaped-graphics/fwd.hh> // sg::context_handle

cc::vector<sr_test::backend_entry>& sr_test::backends()
{
    static cc::vector<backend_entry> entries;
    return entries;
}

// One alias per invocable, so `dev.py test "sr - <name>"` still selects that one test, on every backend.
NX_TEST_SETUP(nx::setup& s)
{
    for (auto const* t : s.invocables_with<sg::context_handle>())
    {
        cc::vector<nx::alias_fragment> fragments;
        for (auto const& b : sr_test::backends())
            if (auto const* driver = s.find_test(b.driver))
                fragments.push_back(nx::alias_fragment{.driver = driver, .section_path = {b.invoke, t->name}});

        if (!fragments.empty())
            s.define_alias(t->name, cc::move(fragments));
    }
}
