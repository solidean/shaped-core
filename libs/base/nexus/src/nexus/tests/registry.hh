#pragma once

#include <nexus/tests/config.hh>

#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <vector>


namespace nx
{
struct test_declaration
{
    std::string name;
    nx::config::cfg test_config;
    std::unique_ptr<std::move_only_function<void()>> function;
    std::source_location location;
};

struct test_registry
{
    std::vector<test_declaration> declarations;

    void add_declaration(std::string name, config::cfg test_config, std::move_only_function<void()> function, std::source_location loc = std::source_location::current());
};

test_registry& get_static_test_registry();

} // namespace nx
