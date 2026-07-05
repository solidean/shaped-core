#include "invoke_tests.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/string.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>

#include <algorithm> // std::sort: stable invocation order (registry order is static-init order)
#include <cstring>   // std::strcmp: order matched tests by source location

bool nx::impl::signatures_equal(cc::span<std::type_index const> a, cc::span<std::type_index const> b)
{
    if (a.size() != b.size())
        return false;
    for (cc::isize i = 0; i < a.size(); ++i)
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

    std::sort(matches.begin(), matches.end(),
              [](test_declaration const* a, test_declaration const* b)
              {
                  if (a->name != b->name)
                      return cc::string_view(a->name) < cc::string_view(b->name);
                  if (int const c = std::strcmp(a->location.file_name(), b->location.file_name()); c != 0)
                      return c < 0;
                  return a->location.line() < b->location.line();
              });

    for (auto const* decl : matches)
    {
        ++result.matched;

        // Reduce to the scopes consistent with this (group, child). The child descends with just those, so a
        // divergent sibling scope can't spuriously match deeper. Unscoped (empty) stays unscoped ⇒ run all.
        cc::vector<cc::vector<cc::string>> child_scopes;
        if (!scopes.empty())
        {
            for (auto const& s : scopes)
                if (permits(s, consumed, name) && permits(s, consumed + 1, decl->name))
                    child_scopes.push_back(s);
            if (child_scopes.empty())
                continue; // this child is scoped out
        }

        // Cycle guard: this invocable is already running further up the chain, so invoking it again would
        // recurse forever. Fail the current test with a clear message instead of overflowing the stack.
        if (is_declaration_active(decl))
        {
            report_invocation_cycle(decl);
            continue;
        }

        test_execution child;
        child.instance.declaration = decl;
        child.invocation_group = cc::string(name);

        run_test_body(child, *config, [&] { decl->invocable_function(values); }, child_scopes, consumed + 2);

        ++result.executed;
        parent->nested.push_back(cc::move(child));
    }

    return result;
}
