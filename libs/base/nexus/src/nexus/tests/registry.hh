#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/string/string.hh>
#include <nexus/fwd.hh>
#include <nexus/tests/config.hh>
#include <nexus/tests/typed_value.hh>

#include <typeindex>

namespace nx
{
struct alias_fragment;
struct test_alias;
struct test_declaration;
struct test_registry;
} // namespace nx

struct nx::test_declaration
{
    cc::string name;
    nx::config::cfg test_config;
    cc::source_location location;

    // Ordinary (nullary) tests: `signature` is empty and `function` is the body.
    cc::unique_function<void()> function;

    // Invocable tests (INVOCABLE_TEST): `signature` is the decayed argument-type list, the invoke_tests join key.
    // `invocable_function` runs the body with args sourced from typed_value slots, and `function` is left invalid.
    // These are inert: a sweep never schedules them, and they run only when a driver calls nx::invoke_tests with a matching signature.
    cc::vector<std::type_index> signature;
    cc::unique_function<void(cc::span<nx::typed_value*>)> invocable_function;

    [[nodiscard]] bool is_invocable() const { return !signature.empty(); }
};

// One runnable target an alias expands to: a driver test, plus the section path that scopes into it.
// For a per-backend invocable the path is {invoke-group, invocable-name}, e.g. {"dx12", "sg - clears backbuffer"}.
// Running the alias then drives just that one instance, under that one backend's driver.
struct nx::alias_fragment
{
    test_declaration const* driver = nullptr;
    cc::vector<cc::string> section_path;
};

// A pseudo test-name standing for a set of scoped runs, defined at startup by NX_TEST_SETUP with full registry access.
// A filter matching an alias name expands into one scheduled instance per fragment.
struct nx::test_alias
{
    cc::string name;
    cc::vector<alias_fragment> fragments;
    cc::source_location location;
};

struct nx::test_registry
{
    cc::vector<test_declaration> declarations;

    // Populated by run_setup_callbacks (from NX_TEST_SETUP bodies) before scheduling/listing.
    cc::vector<test_alias> aliases;

    void add_declaration(cc::string name,
                         config::cfg test_config,
                         cc::unique_function<void()> function,
                         cc::source_location loc = cc::source_location::current());

    void add_invocable_declaration(cc::string name,
                                   config::cfg test_config,
                                   cc::vector<std::type_index> signature,
                                   cc::unique_function<void(cc::span<nx::typed_value*>)> invocable_function,
                                   cc::source_location loc = cc::source_location::current());

    void add_alias(test_alias alias);
};

namespace nx
{

test_registry& get_static_test_registry();

} // namespace nx
