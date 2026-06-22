#include "registry.hh"

namespace nx
{
test_registry& get_static_test_registry()
{
    static test_registry registry;
    return registry;
}

void test_registry::add_declaration(std::string name, config::cfg test_config, std::move_only_function<void()> function, std::source_location loc)
{
    declarations.push_back(test_declaration{
        .name = std::move(name),
        .test_config = test_config,
        .function = std::make_unique<std::move_only_function<void()>>(std::move(function)),
        .location = loc,
    });
}

} // namespace nx
