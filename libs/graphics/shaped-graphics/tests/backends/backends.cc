#include "sg_backends.hh"

#include <nexus/test.hh>
#include <shaped-graphics/fwd.hh> // sg::context_handle

// Wires the sg API up to the alias mechanism: for every INVOCABLE_TEST taking a context_handle, define an
// alias of the same name that expands to one scoped run per available backend. So `dev.py test "sg - ..."`
// runs that one API test on dx12, vulkan, … — whichever backends this binary was built with.

cc::vector<sg_test::backend_entry>& sg_test::backends()
{
    static cc::vector<backend_entry> entries;
    return entries;
}

NX_TEST_SETUP(nx::setup& s)
{
    for (auto const* t : s.invocables_with<sg::context_handle>())
    {
        cc::vector<nx::alias_fragment> fragments;
        for (auto const& b : sg_test::backends())
            if (auto const* driver = s.find_test(b.driver))
                fragments.push_back(nx::alias_fragment{.driver = driver, .section_path = {b.invoke, t->name}});

        if (!fragments.empty())
            s.define_alias(t->name, cc::move(fragments));
    }
}
