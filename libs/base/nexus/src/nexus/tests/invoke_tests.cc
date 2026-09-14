#include "invoke_tests.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/asserts.hh>
#include <clean-core/common/compare.hh>
#include <clean-core/string/format.hh>
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

cc::string nx::impl::find_unhonoured_dispatch_config(config::cfg const& child, config::cfg const& slot)
{
    if (child.exclusive_global && !slot.exclusive_global)
        return "exclusive()";

    if (!slot.exclusive_global)
    {
        for (auto i = 0; i < child.exclusion_tag_count && i < config::max_exclusion_tags; ++i)
        {
            auto const tag = cc::string_view(child.exclusion_tags[i]);
            auto held = false;
            for (auto j = 0; j < slot.exclusion_tag_count && j < config::max_exclusion_tags; ++j)
                held |= cc::string_view(slot.exclusion_tags[j]) == tag;
            if (!held)
                return cc::format("exclusive(\"{}\")", tag);
        }
    }

    if (child.main_thread && !slot.main_thread)
        return "main_thread";

    auto const child_is_default
        = child.scheduler == config::scheduler_mode::shared && child.ambient == config::ambient_mode::multi_threaded;
    auto const same_mode
        = child.scheduler == slot.scheduler && child.ambient == slot.ambient
       && (child.scheduler != config::scheduler_mode::own_pool || child.scheduler_threads == slot.scheduler_threads);
    if (!child_is_default && !same_mode)
    {
        if (child.scheduler == config::scheduler_mode::own_pool)
            return cc::format("own_pool({})", child.scheduler_threads);
        if (child.scheduler == config::scheduler_mode::none && child.ambient == config::ambient_mode::single_threaded)
            return "singlethreaded";
        if (child.scheduler == config::scheduler_mode::none && child.ambient == config::ambient_mode::none)
            return "no_scheduler";
        return "a non-default scheduler mode";
    }

    return {};
}

cc::string nx::impl::find_unhonoured_async_dispatch_config(config::cfg const& child,
                                                           config::cfg const& slot,
                                                           bool chain_holds_tags,
                                                           bool in_parallel)
{
    if (child.exclusive_global && !slot.exclusive_global)
        return "exclusive(), which needs an exclusive() driver";
    if (child.exclusive_global && in_parallel)
        return "exclusive(), which a parallel invocation cannot give it among its siblings";

    if (child.exclusion_tag_count > 0 && chain_holds_tags)
        return cc::format("exclusive(\"{}\") while the invoking chain already holds a tag; the driver may hold tags or "
                          "its "
                          "children may, never both",
                          cc::string_view(child.exclusion_tags[0]));

    // Exclusion and main_thread are arranged by the invocation, so only the scheduler mode is left to the sync rule.
    auto mode_only_child = child;
    mode_only_child.exclusive_global = false;
    mode_only_child.exclusion_tag_count = 0;
    mode_only_child.main_thread = false;
    auto const mode = find_unhonoured_dispatch_config(mode_only_child, slot);
    if (!mode.empty())
        return cc::format("{}, which the driver has to declare as well", mode);

    return {};
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

    // Checked against the MATCHED set, before any -c scoping: a driver that silently skipped its async children whenever
    // a filter happened to select only sync ones would be wrong in a way nothing reports.
    for (auto const* decl : matches)
        CC_ASSERTS(!decl->is_async(), cc::format("nx::invoke_tests: \"{}\" is an ASYNC_INVOCABLE_TEST, which a "
                                                 "synchronous invocation cannot "
                                                 "run — make the driver an ASYNC_TEST and co_await "
                                                 "nx::async_invoke_tests_in_sequence or "
                                                 "nx::async_invoke_tests_in_parallel",
                                                 decl->name));

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

        // A child's own scheduling asks create no node, so only the slot it runs in can honour them.
        // Dispatch is discovered at runtime, so this is the one place a missing ask can be caught at all.
        if (auto const* const slot = current_slot_declaration(); slot != nullptr)
        {
            auto const unhonoured = find_unhonoured_dispatch_config(decl->test_config, slot->test_config);
            auto const* const caller = parent->instance.declaration;
            auto const via = caller == slot ? cc::string() : cc::format(" (dispatching through \"{}\")", caller->name);
            CC_ASSERTS(unhonoured.empty(),
                       cc::format("nx::invoke_tests: \"{}\" declares {}, but \"{}\"{} does not hold it — a dispatched "
                                  "child runs in the schedule slot of the test it was reached from, so add {} there",
                                  decl->name, unhonoured, slot->name, via, unhonoured));
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
