#include "registry.hh"

#include <clean-core/common/utility.hh>

namespace nx
{
test_registry& get_static_test_registry()
{
    static test_registry registry;
    return registry;
}

void test_registry::add_declaration(cc::string name,
                                    config::cfg test_config,
                                    cc::unique_function<void()> function,
                                    cc::source_location loc)
{
    declarations.push_back(test_declaration{
        .name = cc::move(name),
        .test_config = test_config,
        .function = cc::move(function),
        .location = loc,
    });
}

} // namespace nx
