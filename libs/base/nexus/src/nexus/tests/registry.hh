#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/string/string.hh>
#include <nexus/tests/config.hh>
#include <nexus/tests/typed_value.hh>

#include <typeindex>


namespace nx
{
struct test_declaration
{
    cc::string name;
    nx::config::cfg test_config;
    cc::source_location location;

    // Ordinary (nullary) tests: `signature` is empty and `function` is the body.
    cc::unique_function<void()> function;

    // Invocable tests (INVOCABLE_TEST): `signature` is the decayed argument-type list (the invoke_tests
    // join key) and `invocable_function` runs the body with args sourced from typed_value slots. These are
    // inert — a sweep never schedules them; they run only when a driver calls nx::invoke_tests with a
    // matching signature. For an invocable decl `function` is left invalid.
    cc::vector<std::type_index> signature;
    cc::unique_function<void(cc::span<nx::typed_value*>)> invocable_function;

    [[nodiscard]] bool is_invocable() const { return !signature.empty(); }
};

struct test_registry
{
    cc::vector<test_declaration> declarations;

    void add_declaration(cc::string name,
                         config::cfg test_config,
                         cc::unique_function<void()> function,
                         cc::source_location loc = cc::source_location::current());

    void add_invocable_declaration(cc::string name,
                                   config::cfg test_config,
                                   cc::vector<std::type_index> signature,
                                   cc::unique_function<void(cc::span<nx::typed_value*>)> invocable_function,
                                   cc::source_location loc = cc::source_location::current());
};

test_registry& get_static_test_registry();

} // namespace nx
