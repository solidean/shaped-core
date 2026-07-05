#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <nexus/tests/registry.hh>


void nx::impl::register_test(char const* name, config::cfg test_config, void (*fn)(), cc::source_location loc)
{
    nx::get_static_test_registry().add_declaration(name, test_config, fn, loc);
}

void nx::impl::register_invocable_test(char const* name,
                                       config::cfg test_config,
                                       cc::vector<std::type_index> signature,
                                       cc::unique_function<void(cc::span<nx::typed_value*>)> fn,
                                       cc::source_location loc)
{
    nx::get_static_test_registry().add_invocable_declaration(name, test_config, cc::move(signature), cc::move(fn), loc);
}
