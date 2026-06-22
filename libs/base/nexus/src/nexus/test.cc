#include <nexus/test.hh>
#include <nexus/tests/registry.hh>


void nx::impl::register_test(char const* name, config::cfg test_config, void (*fn)(), std::source_location loc)
{
    nx::get_static_test_registry().add_declaration(name, test_config, fn, loc);
}
