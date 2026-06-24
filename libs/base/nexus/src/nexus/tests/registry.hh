#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/string/string.hh>
#include <nexus/tests/config.hh>


namespace nx
{
struct test_declaration
{
    cc::string name;
    nx::config::cfg test_config;
    cc::unique_function<void()> function;
    cc::source_location location;
};

struct test_registry
{
    cc::vector<test_declaration> declarations;

    void add_declaration(cc::string name,
                         config::cfg test_config,
                         cc::unique_function<void()> function,
                         cc::source_location loc = cc::source_location::current());
};

test_registry& get_static_test_registry();

} // namespace nx
