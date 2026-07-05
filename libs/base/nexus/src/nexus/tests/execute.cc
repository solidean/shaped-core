#include "execute.hh"

#include <clean-core/common/assert-handler.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string.hh>
#include <nexus/tests/check.hh>
#include <nexus/tests/section.hh>

#include <chrono>        // std::chrono: no cc timing yet
#include <cstdio>        // std::fputs / std::fwrite: crash-context hook writes to stderr without allocating
#include <string>        // std::string: key type for the std::unordered_map below
#include <unordered_map> // std::unordered_map: cc::map is not implemented yet


namespace nx
{
namespace
{
struct test_section
{
    std::unordered_map<std::string, cc::unique_ptr<test_section>> subsections;
    cc::vector<test_section*> subsections_ordered;

    test_section* next_open_section = nullptr;
    bool is_done = false;
    int last_visited_in_exec = -1;
    cc::source_location location;
    cc::string name;

    // associated stats
    int executed_checks = 0;
    int failed_checks = 0;
    cc::vector<test_error> errors;
    double duration_seconds = 0.0;

    // accumulates stats for non-leaf sections
    // adds errors for "no checks" and "unreachable subsections"
    // computes "in_considered_failing"
    // populates the result with that
    // require_checks: whether an empty section (no CHECK/REQUIRE anywhere below it) is treated as a failure.
    // True for normal tests — a test with no assertions is almost always a bug. False for manual tests
    // (benchmarks and the like), which legitimately only print.
    void finalize_section_to(test_execution::section& sec, bool require_checks) const
    {
        sec.name = name;
        sec.location = location;
        sec.is_considered_failing = false;
        sec.executed_checks = executed_checks;
        sec.failed_checks = failed_checks;
        sec.errors = errors;
        sec.duration_seconds = duration_seconds;

        // populate and aggregate subsections
        for (auto subsec : subsections_ordered)
        {
            auto& ssec = sec.subsections.emplace_back();
            subsec->finalize_section_to(ssec, require_checks);

            // accumulate
            sec.executed_checks += ssec.executed_checks;
            sec.failed_checks += ssec.failed_checks;
            sec.duration_seconds += ssec.duration_seconds;
            for (auto const& e : ssec.errors)
                sec.errors.push_back(e);
            sec.is_considered_failing |= ssec.is_considered_failing;

            // unreachable section
            if (is_done && !subsec->is_done)
            {
                sec.errors.push_back(test_error{
                    .expr = "unreachable section",
                    .location = subsec->location,
                    .extra_lines = {},
                    .expanded = cc::format("section \"{}\" was discovered but unreachable from parent", subsec->name),
                });
                sec.is_considered_failing = true;
            }
        }

        // we record missing CHECK/REQUIRE for _all_ sections, even intermediate ones
        if (require_checks && sec.executed_checks == 0)
        {
            sec.errors.push_back(test_error{
                .expr = "no CHECK/REQUIRE",
                .location = location,
                .extra_lines = {"This is often a bug and can be silenced via CHECK(true)"},
                .expanded = "test did not contain CHECK/REQUIRE",
            });
            sec.is_considered_failing = true;
        }

        // final checks
        sec.is_considered_failing |= sec.failed_checks > 0;
        sec.is_considered_failing |= !errors.empty();
    }
};

struct test_context
{
    nx::test_execution* execution = nullptr;
    nx::test_schedule_config const* config = nullptr;
    cc::unique_ptr<test_section> root_section;
    cc::vector<test_section*> curr_section;

    // The effective set of allowed section paths for this context (an instance's grouped alias-fragment paths,
    // or the run-global config.section_filters as one scope). A section/dispatch runs if it matches ANY scope.
    // A dispatched child inherits the reduced subset consistent with its path (see invoke_tests). Spans point
    // into storage that outlives the run (the execution's instance, the config, or the dispatcher's locals).
    cc::span<cc::vector<cc::string> const> section_scopes;

    // How many leading scope segments this context's path already consumed (see run_test_body): a nested
    // dispatched child starts matching sections at scope[filter_offset].
    int filter_offset = 0;

    // current stats
    int executed_checks = 0;
    int failed_checks = 0;
    cc::vector<test_error> errors;

    // the first section we close becomes the current "leaf" section
    // after a run, all checks & errors are associated to the current leaf
    test_section* leaf_section = nullptr;

    int exec_count = 0;
};

// Exception thrown when a REQUIRE fails
struct test_require_failed
{
};

// Exception thrown when a SKIP is encountered
struct test_skipped
{
};

struct test_duplicate_section
{
    cc::string name;
    cc::source_location location;
};

thread_local cc::vector<test_context> g_context_stack;

// Registry that nx::invoke_tests queries during the current execute_tests run (so a run over a local registry
// dispatches within that same registry). Saved/restored around execute_tests to support nesting.
thread_local nx::test_registry const* g_active_registry = nullptr;

// Plain globals tracking the currently running test, read by the crash-context hook
// (report_running_test). Kept as a raw pointer + length so the hook needs no allocation
// and no cc::string access. Updated just before each test/section runs.
char const* g_running_test_data = nullptr;
int g_running_test_size = 0;
int g_running_test_section = 0;

// When non-null, check results are tallied here instead of being recorded on the active test
// (see nx::impl::scoped_check_capture). Only the innermost installed sink is active.
thread_local nx::impl::check_capture_sink* g_check_capture = nullptr;

// Does one scope (a single filter path) permit `section_name` opening at the current section path?
bool scope_allows(cc::span<test_section* const> curr_section,
                  cc::string_view section_name,
                  cc::span<cc::string const> scope,
                  int filter_offset)
{
    // A dispatched child's path already consumed `filter_offset` leading segments (the dispatch group + child
    // name); its own sections match against the remainder. Consumed past the end ⇒ everything below is allowed.
    if (filter_offset >= scope.size())
        return true;
    auto const filter = scope.subspan(filter_offset);

    // index 0 is the root, which carries no filterable name
    auto const path = curr_section.subspan(1);

    auto const check_size = cc::min(path.size(), filter.size());
    for (cc::isize i = 0; i < check_size; ++i)
    {
        if (path[i]->name != filter[i])
            return false;
    }

    // sections past the current path must match the next filter element
    if (path.size() < filter.size())
    {
        if (section_name != filter[path.size()])
            return false;
    }

    return true;
}

// A section is allowed if it matches ANY scope (OR semantics). No scopes ⇒ everything is allowed.
bool is_section_allowed(cc::span<test_section* const> curr_section,
                        cc::string_view section_name,
                        cc::span<cc::vector<cc::string> const> section_scopes,
                        int filter_offset)
{
    if (section_scopes.empty())
        return true;

    for (auto const& scope : section_scopes)
        if (scope_allows(curr_section, section_name, scope, filter_offset))
            return true;

    return false;
}

void test_execute_begin(nx::test_execution& execution,
                        nx::test_schedule_config const& config,
                        cc::span<cc::vector<cc::string> const> section_scopes,
                        int filter_offset)
{
    g_context_stack.push_back(test_context{
        .execution = &execution,
        .config = &config,
        .root_section = cc::make_unique<test_section>(),
        .section_scopes = section_scopes,
        .filter_offset = filter_offset,
    });
    g_context_stack.back().root_section->location = execution.instance.declaration->location;
    g_context_stack.back().curr_section.push_back(g_context_stack.back().root_section.get());
}

void test_execute_end()
{
    CC_ASSERT(!g_context_stack.empty(), "should be properly balanced");

    auto& ctx = g_context_stack.back();
    CC_ASSERT(ctx.execution != nullptr, "should always have a valid execution");

    // Only normal tests must contain a CHECK/REQUIRE; manual tests and guide benchmarks may legitimately
    // have none — don't flag that as failure. A driver that dispatches parametrized tests (nested non-empty)
    // is also exempt: its assertions live in the dispatched children, not its own body.
    bool const require_checks = ctx.execution->instance.declaration->test_config.bucket == config::test_bucket::normal
                             && ctx.execution->nested.empty();
    ctx.root_section->finalize_section_to(ctx.execution->root, require_checks);

    g_context_stack.remove_back();
}

// Operator to string conversion
char const* op_to_string(impl::cmp_op op)
{
    using namespace impl;
    switch (op)
    {
    case cmp_op::none:
        return "";
    case cmp_op::less:
        return "<";
    case cmp_op::less_equal:
        return "<=";
    case cmp_op::greater:
        return ">";
    case cmp_op::greater_equal:
        return ">=";
    case cmp_op::equal:
        return "==";
    case cmp_op::not_equal:
        return "!=";
    case cmp_op::throws:
        return "throws";
    case cmp_op::throws_as:
        return "throws_as";
    case cmp_op::assert_fail:
        return "assert_fail";
    case cmp_op::asserts:
        return "asserts";
    case cmp_op::skip:
        return "skip";
    }
    return "?";
}
} // namespace
} // namespace nx


nx::impl::raii_section_opener nx::impl::test_open_section(cc::string name, cc::source_location location)
{
    auto& ctx = g_context_stack.back();

    auto& curr_sec = *ctx.curr_section.back();

    // check section filter if provided
    if (!is_section_allowed(ctx.curr_section, name, ctx.section_scopes, ctx.filter_offset))
    {
        // we do this so early that the subsection is not even actually created
        return raii_section_opener(false);
    }

    // new subsection? (std::unordered_map is keyed by std::string, so bridge the name)
    auto& subsec = curr_sec.subsections[std::string(name.data(), name.size())];
    if (subsec == nullptr)
    {
        subsec = cc::make_unique<test_section>();
        subsec->name = name;
        subsec->location = location;
        curr_sec.subsections_ordered.push_back(subsec.get());
    }

    // section opened twice in the same run
    if (subsec->last_visited_in_exec == ctx.exec_count)
        throw test_duplicate_section{
            .name = cc::move(name),
            .location = location,
        };
    subsec->last_visited_in_exec = ctx.exec_count;

    // don't execute more sections if a leaf was already executed
    if (ctx.leaf_section != nullptr)
    {
        // but note down that parent could continue here
        curr_sec.next_open_section = subsec.get();
        return raii_section_opener(false);
    }

    // don't execute sections that are fully done
    if (subsec->is_done)
        return raii_section_opener(false);

    // .. otherwise enter it
    ctx.curr_section.push_back(subsec.get());
    subsec->next_open_section = nullptr;
    return raii_section_opener(true);
}

nx::impl::raii_section_opener::raii_section_opener(bool is_opened) : _is_opened(is_opened)
{
}

nx::impl::raii_section_opener::~raii_section_opener()
{
    if (_is_opened)
    {
        auto& ctx = g_context_stack.back();
        auto& subsec = *ctx.curr_section.back();

        CC_ASSERT(ctx.curr_section.size() >= 2, "should always have at least this + root on the stack");

        // if after the section we have no subsecs => found & executed a leaf!
        // (no next open, might have unreachable still)
        // also applies to our way back up
        if (subsec.next_open_section == nullptr)
        {
            if (ctx.leaf_section == nullptr)
                ctx.leaf_section = &subsec;
            subsec.is_done = true;
        }
        else
        {
            // make sure parent knows that children have open sections
            ctx.curr_section[ctx.curr_section.size() - 2]->next_open_section = subsec.next_open_section;
        }

        ctx.curr_section.remove_back();
    }
}

nx::impl::scoped_check_capture::scoped_check_capture(check_capture_sink& sink)
{
    // nested captures are allowed; the innermost sink wins. We do not chain on purpose:
    // the fuzz engine never nests captures, and a flat pointer keeps the hot path cheap.
    CC_ASSERT(g_check_capture == nullptr, "nested check captures are not supported");
    g_check_capture = &sink;
}

nx::impl::scoped_check_capture::~scoped_check_capture()
{
    g_check_capture = nullptr;
}

nx::test_registry const* nx::impl::active_registry()
{
    return g_active_registry;
}

bool nx::impl::is_declaration_active(nx::test_declaration const* decl)
{
    for (auto const& ctx : g_context_stack)
        if (ctx.execution != nullptr && ctx.execution->instance.declaration == decl)
            return true;
    return false;
}

void nx::impl::report_invocation_cycle(nx::test_declaration const* decl)
{
    CC_ASSERT(!g_context_stack.empty(), "must be called within a running test");
    auto const name = decl != nullptr ? cc::string_view(decl->name) : cc::string_view("<null>");
    g_context_stack.back().errors.push_back(nx::test_error{
        .expr = cc::format("nx::invoke_tests cycle: \"{}\" is already running", name),
        .location = decl != nullptr ? decl->location : cc::source_location::current(),
        .extra_lines = {"an invocable must not (transitively) invoke itself"},
        .expanded = cc::format("invocation cycle: \"{}\" would recurse into itself", name),
    });
}

nx::test_execution* nx::impl::current_execution()
{
    if (g_context_stack.empty())
        return nullptr;
    return g_context_stack.back().execution;
}

nx::test_schedule_config const* nx::impl::current_config()
{
    if (g_context_stack.empty())
        return nullptr;
    return g_context_stack.back().config;
}

int nx::impl::current_filter_consumed()
{
    if (g_context_stack.empty())
        return 0;
    auto& ctx = g_context_stack.back();
    // curr_section always holds at least the root (which carries no filterable name)
    return ctx.filter_offset + int(ctx.curr_section.size()) - 1;
}

cc::span<cc::vector<cc::string> const> nx::impl::current_section_scopes()
{
    if (g_context_stack.empty())
        return {};
    return g_context_stack.back().section_scopes;
}

void nx::impl::report_running_test() noexcept
{
    if (g_running_test_data == nullptr || g_running_test_size <= 0)
    {
        std::fputs("running test: <none>\n", stderr);
        return;
    }
    std::fputs("running test: \"", stderr);
    std::fwrite(g_running_test_data, 1, size_t(g_running_test_size), stderr);
    std::fputc('"', stderr);
    if (g_running_test_section > 0)
        std::fprintf(stderr, " (section %d)", g_running_test_section);
    std::fputc('\n', stderr);
}

void nx::impl::record_metric(cc::string_view name, double value, cc::string_view unit, bool higher_is_better)
{
    if (g_context_stack.empty())
        return; // no active test — recording is a no-op outside a test body

    auto* const execution = g_context_stack.back().execution;
    if (execution == nullptr)
        return;

    execution->metrics.push_back(nx::recorded_metric{cc::string(name), value, cc::string(unit), higher_is_better});
}

void nx::impl::report_check_result(check_kind kind,
                                   cmp_op op,
                                   cc::string expr,
                                   bool passed,
                                   cc::vector<cc::string> extra_lines,
                                   cc::source_location location)
{
    // Capture mode: a tool (e.g. the fuzz engine) is driving user code that is expected to fail
    // often. Tally the outcome and suppress both the host-test side effects and the control-flow
    // throws (REQUIRE/SKIP), so a single failing operation does not abort or pollute the host test.
    if (g_check_capture != nullptr)
    {
        auto& sink = *g_check_capture;
        ++sink.executed;
        if (op == cmp_op::skip)
            return;
        if (!passed)
        {
            ++sink.failed;
            if (kind == check_kind::require || op == cmp_op::assert_fail)
                sink.require_failed = true;
            if (sink.first_message.empty())
            {
                cc::string msg = expr;
                for (auto const& line : extra_lines)
                    if (!line.empty())
                    {
                        msg += " | ";
                        msg += line;
                    }
                sink.first_message = cc::move(msg);
            }
        }
        return;
    }

    if (g_context_stack.empty())
        return; // No active test context

    auto& ctx = g_context_stack.back();

    // Increment executed checks
    ++ctx.executed_checks;

    // If this is a SKIP, throw to abort test execution (counts as success)
    if (op == cmp_op::skip)
        throw test_skipped{};

    // If the check failed, record it
    if (!passed)
    {
        ++ctx.failed_checks;

        cc::string expanded;
        switch (op)
        {
        case cmp_op::none:
            expanded = cc::format("'{}' failed", expr);
            break;

        case cmp_op::less:
        case cmp_op::less_equal:
        case cmp_op::greater:
        case cmp_op::greater_equal:
        case cmp_op::equal:
        case cmp_op::not_equal:
            if (extra_lines.size() >= 2)
                expanded = cc::format("{} {} {}", extra_lines[0], op_to_string(op), extra_lines[1]);
            else
                expanded = "(could not capture expressions)";
            break;

        case cmp_op::throws:
            if (!extra_lines.empty())
                expanded = extra_lines[0];
            else
                expanded = "expression did not throw an exception (but should have)";
            break;

        case cmp_op::throws_as:
            if (!extra_lines.empty())
                expanded = extra_lines[0];
            else
                expanded = "expression did not throw an exception (but should have) or threw the wrong type";
            break;

        case cmp_op::assert_fail:
            if (!extra_lines.empty())
                expanded = extra_lines[0];
            else
                expanded = "assertion failed during test";
            break;

        case cmp_op::asserts:
            if (!extra_lines.empty())
                expanded = extra_lines[0];
            else
                expanded = "assertion should have failed (but did not)";
            break;

        case cmp_op::skip:
            CC_UNREACHABLE("skip should not produce a test error");
        }

        // Add test error
        ctx.errors.push_back(test_error{
            .expr = std::move(expr),
            .location = location,
            .extra_lines = std::move(extra_lines),
            .expanded = std::move(expanded),
        });

        // If this was a REQUIRE, throw exception to abort test execution
        if (kind == check_kind::require)
            throw test_require_failed{};
    }
}

bool nx::test_execution::is_considered_failing() const
{
    if (root.is_considered_failing)
        return true;
    for (auto const& child : nested)
        if (child.is_considered_failing())
            return true;
    return false;
}

namespace nx
{
namespace
{
int total_tests_of(nx::test_execution const& exec)
{
    int n = 1;
    for (auto const& child : exec.nested)
        n += total_tests_of(child);
    return n;
}
int failed_tests_of(nx::test_execution const& exec)
{
    // A dispatched child counts as its own test; the driver counts only if its own tree fails.
    int n = exec.root.is_considered_failing ? 1 : 0;
    for (auto const& child : exec.nested)
        n += failed_tests_of(child);
    return n;
}
int total_checks_of(nx::test_execution const& exec)
{
    int n = exec.root.executed_checks;
    for (auto const& child : exec.nested)
        n += total_checks_of(child);
    return n;
}
int failed_checks_of(nx::test_execution const& exec)
{
    int n = exec.root.failed_checks;
    for (auto const& child : exec.nested)
        n += failed_checks_of(child);
    return n;
}
} // namespace
} // namespace nx

int nx::test_schedule_execution::count_total_tests() const
{
    int total = 0;
    for (auto const& exec : executions)
        total += total_tests_of(exec);
    return total;
}

int nx::test_schedule_execution::count_failed_tests() const
{
    int failed = 0;
    for (auto const& exec : executions)
        failed += failed_tests_of(exec);
    return failed;
}

int nx::test_schedule_execution::count_total_checks() const
{
    int total = 0;
    for (auto const& exec : executions)
        total += total_checks_of(exec);
    return total;
}

int nx::test_schedule_execution::count_failed_checks() const
{
    int failed = 0;
    for (auto const& exec : executions)
        failed += failed_checks_of(exec);
    return failed;
}

void nx::impl::run_test_body(nx::test_execution& execution,
                             nx::test_schedule_config const& config,
                             cc::function_ref<void()> body,
                             cc::span<cc::vector<cc::string> const> section_scopes,
                             int filter_offset)
{
    CC_ASSERT(execution.instance.declaration != nullptr, "instances must be valid");
    auto const& decl = *execution.instance.declaration;

    // Set up test context for check reporting
    test_execute_begin(execution, config, section_scopes, filter_offset);

    // Execute the test body, re-running it once per section-exploration pass
    auto section_num = 0;
    auto should_continue = true;
    while (should_continue)
    {
        // CAUTION: a test is allowed to run nested tests, thus growing the context stack here
        {
            auto& ctx = g_context_stack.back();
            ctx.exec_count++;
            ctx.leaf_section = nullptr;
            ctx.root_section->next_open_section = nullptr;
        }

        // publish the running test for the crash-context hook (points a fatal fault at this test)
        g_running_test_data = decl.name.data();
        g_running_test_size = int(decl.name.size());
        g_running_test_section = section_num;

        if (config.verbose)
        {
            if (section_num == 0)
                cc::println("  - start \"{}\"", decl.name);
            else
                cc::println("  - start \"{}\" section {}", decl.name, section_num);
        }
        section_num++;
        auto const t_section_start = std::chrono::high_resolution_clock::now();

        try
        {
            auto _ = cc::impl::scoped_assertion_handler(
                [](cc::impl::assertion_info const& info)
                {
                    // failing assertion has same semantics as REQUIRE -> it aborts
                    nx::impl::report_check_result(impl::check_kind::require, impl::cmp_op::assert_fail, info.expression,
                                                  false, {info.message}, info.location);
                });

            body();
        }
        catch (test_require_failed const&) // NOLINT(bugprone-empty-catch)
        {
            // REQUIRE failure already logged in report_check_result, this catch
            // only serves to abort test execution without treating it as a further error
        }
        catch (test_skipped const&) // NOLINT(bugprone-empty-catch)
        {
            // SKIP already counted as a successful check in report_check_result, this catch
            // only serves to abort test execution
        }
        catch (test_duplicate_section const& e)
        {
            g_context_stack.back().errors.push_back(test_error{
                .expr = cc::format("duplicate section: \"{}\"", e.name),
                .location = e.location,
                .extra_lines = {},
                .expanded = cc::format("duplicate section: \"{}\"", e.name),
            });
            should_continue = false; // wrong use of test framework
        }
        catch (std::exception const& e)
        {
            g_context_stack.back().errors.push_back(test_error{
                .expr = cc::format("uncaught exception: {}", e.what()),
                .location = decl.location,
                .extra_lines = {},
                .expanded = cc::format("uncaught exception: {}", e.what()),
            });
        }
        catch (...)
        {
            g_context_stack.back().errors.push_back(test_error{
                .expr = "uncaught unknown exception",
                .location = decl.location,
                .extra_lines = {},
                .expanded = "uncaught unknown exception",
            });
        }

        // associate stats & errors with leaf
        auto sec = g_context_stack.back().leaf_section;
        if (sec == nullptr)
            sec = g_context_stack.back().root_section.get();
        CC_ASSERT(sec != nullptr, "should always have a leaf section");
        {
            auto& ctx = g_context_stack.back();
            auto const t_section_end = std::chrono::high_resolution_clock::now();
            sec->duration_seconds = std::chrono::duration<double>(t_section_end - t_section_start).count();
            sec->executed_checks = cc::exchange(ctx.executed_checks, 0);
            sec->failed_checks = cc::exchange(ctx.failed_checks, 0);
            sec->errors = cc::exchange(ctx.errors, {});
        }

        // no new sections to execute? we're done
        CC_ASSERT(!g_context_stack.empty(), "test context should still be valid");
        if (g_context_stack.back().root_section->next_open_section == nullptr)
        {
            // so it's not marked as unreachable
            g_context_stack.back().root_section->is_done = true;

            // .. and we're done!
            should_continue = false;
        }
    }

    // Clean up test context (finalizes execution.root)
    test_execute_end();

    if (config.verbose)
    {
        double const duration_ms = execution.root.duration_seconds * 1000.0;
        cc::println("    ... in {:.2f} ms ({} checks, {} failed checks, {} errors)", duration_ms,
                    execution.root.executed_checks, execution.root.failed_checks, execution.root.errors.size());
    }
}

nx::test_schedule_execution nx::execute_tests(test_schedule const& schedule, test_schedule_config const& config)
{
    test_schedule_execution result;

    if (config.verbose)
    {
        cc::println("executing {} tests", schedule.instances.size());
    }

    // Make this schedule's registry the one nx::invoke_tests queries (save/restore for nested execute_tests).
    auto* const prev_registry = g_active_registry;
    g_active_registry = schedule.registry;
    CC_DEFER
    {
        g_active_registry = prev_registry;
    };

    for (auto const& instance : schedule.instances)
    {
        CC_ASSERT(instance.declaration != nullptr, "instances must be valid");
        CC_ASSERT(instance.declaration->function.is_valid(), "ordinary instances must have a nullary body");
        test_execution execution;
        execution.instance = instance;

        // Effective scopes: the instance's own grouped set (alias-expanded), else the run-global -c path
        // presented as a single scope, else none (run everything). All three storages outlive run_test_body.
        cc::vector<cc::vector<cc::string>> global_scope;
        cc::span<cc::vector<cc::string> const> section_scopes;
        if (!execution.instance.section_scopes.empty())
            section_scopes = execution.instance.section_scopes;
        else if (!config.section_filters.empty())
        {
            global_scope.push_back(config.section_filters);
            section_scopes = global_scope;
        }

        nx::impl::run_test_body(
            execution, config, [&] { instance.declaration->function(); }, section_scopes,
            /*filter_offset=*/0);

        result.executions.push_back(cc::move(execution));
    }

    return result;
}
