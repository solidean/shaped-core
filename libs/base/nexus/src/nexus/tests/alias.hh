#pragma once

#include <clean-core/common/traits.hh> // cc::arg_types_of / cc::signature
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/invoke_tests.hh> // nx::impl::signatures_equal
#include <nexus/tests/registry.hh>

#include <typeindex>

namespace nx
{
// Handle passed to an NX_TEST_SETUP body. It reads the registry and defines aliases against it. Aliases
// land on the wrapped registry (the static registry for a real run; a local one in meta-tests).
struct setup
{
    explicit setup(test_registry& registry) : _registry(&registry) {}

    // All registered declarations (ordinary + invocable), in registration order.
    cc::span<test_declaration const> tests() const { return _registry->declarations; }

    // The declaration named `name`, or nullptr. Used to resolve a driver for an alias fragment.
    test_declaration const* find_test(cc::string_view name) const;

    // Every invocable test whose decayed signature is exactly `Args...` (the same join key nx::invoke_tests
    // uses). Typically Args is the driver's argument, e.g. invocables_with<sg::context_handle>().
    template <class... Args>
    cc::vector<test_declaration const*> invocables_with() const
    {
        auto const sig = cc::arg_types_of(cc::signature<void(Args...)>{});
        cc::vector<test_declaration const*> out;
        for (auto const& decl : _registry->declarations)
            if (decl.is_invocable() && impl::signatures_equal(decl.signature, sig))
                out.push_back(&decl);
        return out;
    }

    // Registers an alias `name` expanding to `fragments` (each a driver + section path scoping into it).
    void define_alias(cc::string name,
                      cc::vector<alias_fragment> fragments,
                      cc::source_location loc = cc::source_location::current());

private:
    test_registry* _registry;
};

namespace impl
{
// Registers a startup callback (via NX_TEST_SETUP). Callbacks run once, before any listing or scheduling.
void register_setup(void (*fn)(nx::setup&), cc::source_location loc);
} // namespace impl

// Runs every registered NX_TEST_SETUP callback against `registry`, (re)building its aliases. Idempotent:
// clears registry.aliases first, then sorts aliases and their fragments for a stable, reproducible listing.
void run_setup_callbacks(test_registry& registry);

} // namespace nx
