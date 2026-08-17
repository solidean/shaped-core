#include "invoke_tests.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/compare.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/fwd.hh> // also what puts the bare sized aliases in scope inside nx
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>


bool nx::impl::signatures_equal(cc::span<std::type_index const> a, cc::span<std::type_index const> b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

nx::invocation_result nx::impl::invoke_tests_impl(cc::string_view name,
                                                  cc::span<std::type_index const> signature,
                                                  cc::span<nx::typed_value*> values)
{
    invocation_result result;

    auto* const parent = current_execution();
    auto const* const config = current_config();
    CC_ASSERT(parent != nullptr && config != nullptr, "nx::invoke_tests must be called from within a running test");

    // How many scope segments this path already consumed (ancestors + our own open sections). The invocation
    // group is the next segment, the child name the one after; the child's own sections follow.
    int const consumed = current_filter_consumed();

    // The effective scopes of the running instance (the grouped alias-fragment paths, or the global -c as one
    // scope). A dispatch runs if it matches ANY scope; the child descends with just the consistent subset.
    auto const scopes = current_section_scopes();

    // A scope permits segment `seg` at index `idx` when it is exhausted there (matches everything below) or
    // names `seg`.
    auto const permits = [](cc::span<cc::string const> s, int idx, cc::string_view seg)
    { return idx >= int(s.size()) || cc::string_view(s[idx]) == seg; };

    // Whole invocation group scoped out? (unscoped runs it; otherwise some scope must permit `name` here)
    if (!scopes.empty())
    {
        bool any_group = false;
        for (auto const& s : scopes)
            if (permits(s, consumed, name))
            {
                any_group = true;
                break;
            }
        if (!any_group)
            return result;
    }

    // Collect signature matches from the active registry (the run's own registry; static registry for a
    // normal run), sorted for a stable, reproducible order (registry order is static-init order).
    auto const* registry = active_registry();
    if (registry == nullptr)
        registry = &get_static_test_registry();

    cc::vector<test_declaration const*> matches;
    for (auto const& decl : registry->declarations)
        if (decl.is_invocable() && signatures_equal(decl.signature, signature))
            matches.push_back(&decl);

    cc::sort(matches, cc::compare_by([](test_declaration const* d) { return cc::string_view(d->name); },
                                     [](test_declaration const* d) { return cc::string_view(d->location.file_name()); },
                                     [](test_declaration const* d) { return d->location.line(); }));

    for (auto const* decl : matches)
    {
        ++result.matched;

        // Reduce to the scopes consistent with this (group, child), and the child descends with just those.
        // A divergent sibling scope then cannot spuriously match deeper, and unscoped (empty) stays unscoped, meaning run all.
        cc::vector<cc::vector<cc::string>> child_scopes;
        if (!scopes.empty())
        {
            for (auto const& s : scopes)
                if (permits(s, consumed, name) && permits(s, consumed + 1, decl->name))
                    child_scopes.push_back(s);
            if (child_scopes.empty())
                continue; // this child is scoped out
        }

        // Cycle guard: this invocable is already running further up the chain, so invoking it again would recurse forever.
        // Fail the current test with a clear message instead of overflowing the stack.
        if (is_declaration_active(decl))
        {
            report_invocation_cycle(decl);
            continue;
        }

        test_execution child;
        child.instance.declaration = decl;
        child.instance.registry = registry; // so a dispatch from inside the child searches the same registry
        child.invocation_group = cc::string(name);

        run_test_body(child, *config, [&] { decl->invocable_function(values); }, child_scopes, consumed + 2);

        ++result.executed;
        parent->nested.push_back(cc::move(child));
    }

    return result;
}
