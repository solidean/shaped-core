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
#include <clean-core/thread/async_ambient.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <nexus/fwd.hh> // also what puts the bare sized aliases in scope inside nx
#include <nexus/tests/check.hh>
#include <nexus/tests/impl/test_ambient.hh>
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
    // Adds errors for "no checks" and "unreachable subsections", computes is_considered_failing, and populates the result with both.
    // `require_checks` says whether an empty section — no CHECK/REQUIRE anywhere below it — counts as a failure.
    // True for a normal test, where no assertions is almost always a bug; false for a manual test or benchmark, which legitimately only prints.
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

    // current stats — the test thread's own, so plain and unsynchronized
    int executed_checks = 0;
    int failed_checks = 0;
    cc::vector<test_error> errors;

    // Checks and metrics reported from anywhere else: a pool worker driving this test's nodes, or a thread it started.
    // They are counted but kept OUT of the section machinery, which is single-threaded replay state — so they merge into the ROOT section at test_execute_end.
    // Splitting them this way is what keeps the thread's own path exactly as fast, and exactly as section-aware, as it was.
    cc::atomic<int> off_thread_executed_checks = {0};
    cc::atomic<int> off_thread_failed_checks = {0};
    cc::mutex<cc::vector<test_error>> off_thread_errors;
    cc::mutex<cc::vector<nx::recorded_metric>> off_thread_metrics;

    // Set once the test's stats have been finalized, after which nothing more may be recorded here.
    // A check arriving later comes from work that outlived the test, and is reported as an orphan naming it — see run_test_body's leak check.
    cc::atomic<bool> is_finished = {false};

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

// Owns the running tests' contexts, innermost last, and nothing more.
// LOOKUP goes through the ambient chain (current_context) — see impl/test_ambient.hh for why a stack read is the wrong answer once work runs off the test's own thread.
// Held by pointer because the ambient links point INTO these contexts, and a nested test growing a vector of values would reallocate them out from under it.
thread_local cc::vector<cc::unique_ptr<test_context>> g_context_stack;

/// The test a check reported right here belongs to, or null if there is none.
/// Reads the ambient chain, so it is correct on a pool worker driving this test's nodes, and equally correct when this thread is driving some OTHER test's stolen node.
test_context* current_context()
{
    return static_cast<test_context*>(cc::async_ambient_lookup(nx::impl::test_ambient_tag()));
}

/// Is a report here part of `ctx`'s own test body — its control flow, its section path, its unsynchronized stats?
///
/// Finer than comparing threads, and deliberately so.
/// A test thread sitting in blocking_get steals and polls OTHER tests' nodes, and those are not its control flow even though they are its thread.
/// Being the innermost context on this thread's stack is exactly the condition that excludes them.
bool is_own_test_body(test_context const* ctx)
{
    return !g_context_stack.empty() && g_context_stack.back().get() == ctx;
}

// Registry that nx::invoke_tests queries during the current execute_tests run (so a run over a local registry
// dispatches within that same registry). Saved/restored around execute_tests to support nesting.
thread_local nx::test_registry const* g_active_registry = nullptr;

// Plain globals tracking the currently running test, read by the crash-context hook report_running_test.
// Kept as a raw pointer plus length so the hook needs no allocation and no cc::string access.
// Updated just before each test or section runs.
char const* g_running_test_data = nullptr;
int g_running_test_size = 0;
int g_running_test_section = 0;

// Checks that found no test context at all — process-global, because by definition no test owns them.
//
// An unattributable check is an ERROR, not a silent drop: it proved nothing, and silence is exactly how a threaded test rots into a no-op nobody notices.
// It is surfaced twice on purpose.
// Printed the moment it happens, because the run may already be over by the time anything drains this, and drained into the run's result so the policy itself is testable and so the run fails.
cc::atomic<int> g_orphan_checks = {0};
cc::mutex<cc::vector<nx::test_error>> g_orphan_errors;

// Contexts of tests that ended with async work still carrying them.
//
// cc keeps the ambient LINK alive for that work, but the value in it is ours, and freeing it would leave the leaked work reporting into freed memory.
// So a leaking test's context is kept instead of destroyed — deliberately, and bounded by the number of tests that leak, each of which is already failing for it.
cc::mutex<cc::vector<cc::unique_ptr<test_context>>> g_leaked_contexts;

// When non-null, check results are tallied here instead of being recorded on the active test
// (see nx::impl::scoped_check_capture). Only the innermost installed sink is active.
thread_local nx::impl::check_capture_sink* g_check_capture = nullptr;

// Does one scope (a single filter path) permit `section_name` opening at the current section path?
bool scope_allows(cc::span<test_section* const> curr_section,
                  cc::string_view section_name,
                  cc::span<cc::string const> scope,
                  int filter_offset)
{
    // A dispatched child's path already consumed `filter_offset` leading segments — the dispatch group plus the child name — so its own sections match the remainder.
    // Consumed past the end means everything below is allowed.
    if (filter_offset >= scope.size())
        return true;
    auto const filter = scope.subspan(filter_offset);

    // index 0 is the root, which carries no filterable name
    auto const path = curr_section.subspan(1);

    auto const check_size = cc::min(path.size(), filter.size());
    for (isize i = 0; i < check_size; ++i)
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

/// Push a context and return it — the caller installs it as the ambient, which is what makes it findable.
test_context* test_execute_begin(nx::test_execution& execution,
                                 nx::test_schedule_config const& config,
                                 cc::span<cc::vector<cc::string> const> section_scopes,
                                 int filter_offset)
{
    // Filled field by field rather than with a designated initializer: the off-thread spill makes test_context immovable, so it has to be built in place.
    auto owned = cc::make_unique<test_context>();
    auto& ctx = *owned;
    ctx.execution = &execution;
    ctx.config = &config;
    ctx.root_section = cc::make_unique<test_section>();
    ctx.section_scopes = section_scopes;
    ctx.filter_offset = filter_offset;
    ctx.root_section->location = execution.instance.declaration->location;
    ctx.curr_section.push_back(ctx.root_section.get());

    g_context_stack.push_back(cc::move(owned));
    return &ctx;
}

/// Finalize the innermost context into its execution and drop it.
/// Reads the stack rather than the ambient: the scope naming this context is already gone by the time we run, precisely so nothing can look it up while it is being destroyed.
///
/// `keep_alive` is set when async work outlived the test and still carries a link naming this context.
/// It is then kept rather than destroyed, so that work reports an orphan instead of writing into freed memory.
void test_execute_end(bool keep_alive)
{
    CC_ASSERT(!g_context_stack.empty(), "should be properly balanced");

    auto& ctx = *g_context_stack.back();
    CC_ASSERT(ctx.execution != nullptr, "should always have a valid execution");

    // Fold in what was reported off the test's own thread.
    // The root section, not the leaf: off-thread work does not follow the section replay, so there is no meaningful leaf to blame it on.
    ctx.root_section->executed_checks += ctx.off_thread_executed_checks.load(cc::memory_order_acquire);
    ctx.root_section->failed_checks += ctx.off_thread_failed_checks.load(cc::memory_order_acquire);
    ctx.off_thread_errors.lock(
        [&](cc::vector<test_error>& errors)
        {
            ctx.root_section->errors.push_back_range(cc::move(errors));
            errors.clear(); // push_back_range moves the elements out but leaves the husks behind
        });
    ctx.off_thread_metrics.lock(
        [&](cc::vector<nx::recorded_metric>& metrics)
        {
            ctx.execution->metrics.push_back_range(cc::move(metrics));
            metrics.clear();
        });

    // Only a normal test must contain a CHECK/REQUIRE; a manual test or guide benchmark may legitimately have none.
    // A driver that dispatches parametrized tests (nested non-empty) is exempt too, since its assertions live in the dispatched children rather than its own body.
    bool const require_checks = ctx.execution->instance.declaration->test_config.bucket == config::test_bucket::normal
                             && ctx.execution->nested.empty();
    ctx.root_section->finalize_section_to(ctx.execution->root, require_checks);

    ctx.is_finished.store(true, cc::memory_order_release);

    auto owned = cc::move(g_context_stack.back());
    g_context_stack.remove_back();
    if (keep_alive)
        g_leaked_contexts.lock([&](cc::vector<cc::unique_ptr<test_context>>& kept) { kept.push_back(cc::move(owned)); });
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

// The one-line failure text: what the op has to say, then every user annotation appended.
// Both report_check_result paths (capture sink and real test context) go through this, so the two cannot
// drift apart again — appending here is also what carries annotations into the JUnit body and the Catch2
// <Expanded> element without either exporter knowing about them.
cc::string render_expanded(impl::check_result const& r)
{
    using namespace impl;

    cc::string expanded;
    switch (r.op)
    {
    case cmp_op::none:
        expanded = cc::format("'{}' failed", r.expr);
        break;

    case cmp_op::less:
    case cmp_op::less_equal:
    case cmp_op::greater:
    case cmp_op::greater_equal:
    case cmp_op::equal:
    case cmp_op::not_equal:
        if (r.operands_captured)
            expanded = cc::format("{} {} {}", r.lhs, op_to_string(r.op), r.rhs);
        else
            expanded = "(could not capture expressions)";
        break;

    case cmp_op::throws:
        expanded = r.diagnostic.empty() ? cc::string("expression did not throw an exception (but should have)")
                                        : r.diagnostic;
        break;

    case cmp_op::throws_as:
        expanded = r.diagnostic.empty()
                     ? cc::string("expression did not throw an exception (but should have) or threw the wrong type")
                     : r.diagnostic;
        break;

    case cmp_op::assert_fail:
        expanded = r.diagnostic.empty() ? cc::string("assertion failed during test") : r.diagnostic;
        break;

    case cmp_op::asserts:
        expanded = r.diagnostic.empty() ? cc::string("assertion should have failed (but did not)") : r.diagnostic;
        break;

    case cmp_op::skip:
        CC_UNREACHABLE("skip should not produce a test error");
    }

    for (auto const& line : r.extra_lines)
        if (!line.empty())
        {
            expanded += " | ";
            expanded += line;
        }

    return expanded;
}

/// Tally a check that did not come from `ctx`'s own test body, and decide whether it may abort by throwing.
///
/// The counters are the easy half.
/// The hard half is REQUIRE/SKIP, which abort by throwing: inside a poll that throw is contained and correctly terminates the node,
/// but on a thread the test merely started there is no handler between here and the thread function, so it would terminate the process instead.
/// There it degrades to a recorded failure — the only choice that does not trade a reported failure for a dead run.
void report_off_thread_check_result(test_context& ctx, impl::check_result result)
{
    ctx.off_thread_executed_checks.fetch_add(1, cc::memory_order_relaxed);

    bool const is_skip = result.op == impl::cmp_op::skip;
    if (!is_skip && !result.passed)
    {
        ctx.off_thread_failed_checks.fetch_add(1, cc::memory_order_relaxed);

        auto expanded = render_expanded(result);
        ctx.off_thread_errors.lock(
            [&](cc::vector<test_error>& errors)
            {
                errors.push_back(test_error{
                    .expr = cc::move(result.expr),
                    .location = result.location,
                    .extra_lines = cc::move(result.extra_lines),
                    .expanded = cc::move(expanded),
                });
            });
    }

    if (!cc::async_is_polling())
        return; // nothing would catch the throw

    if (is_skip)
        throw test_skipped{};
    if (!result.passed && result.kind == impl::check_kind::require)
        throw test_require_failed{};
}

/// Record a check that belongs to no test, and say so on stderr right away.
/// Never throws: there is no test to abort, and no handler to abort into.
/// `why` explains which flavour of unattributable this is, since "no test at all" and "a test that already ended" are different mistakes.
void report_orphan_check(impl::check_result result, cc::string_view why)
{
    g_orphan_checks.fetch_add(1, cc::memory_order_relaxed);

    auto expanded = render_expanded(result);
    cc::eprintln("nexus: unattributed check at {}:{}: {} ({})", result.location.file_name(), result.location.line(),
                 expanded, why);

    g_orphan_errors.lock(
        [&](cc::vector<nx::test_error>& errors)
        {
            errors.push_back(nx::test_error{
                .expr = cc::move(result.expr),
                .location = result.location,
                .extra_lines = {cc::string(why)},
                .expanded = cc::move(expanded),
            });
        });
}

/// Take everything the sink holds, leaving it empty for whatever runs next.
/// Taking rather than reading is what makes nesting work: an inner execute_tests claims what it produced, so the outer run does not report it twice.
void drain_orphan_checks(nx::test_schedule_execution& into)
{
    into.orphan_checks += g_orphan_checks.exchange(0, cc::memory_order_acq_rel);
    g_orphan_errors.lock(
        [&](cc::vector<nx::test_error>& errors)
        {
            into.orphan_errors.push_back_range(cc::move(errors));
            errors.clear(); // push_back_range moves the elements out but leaves the husks behind
        });
}
} // namespace
} // namespace nx


nx::impl::raii_section_opener nx::impl::test_open_section(cc::string name, cc::source_location location)
{
    auto* const ctx_ptr = current_context();
    CC_ASSERT(ctx_ptr != nullptr, "SECTION must be used inside a running test");
    auto& ctx = *ctx_ptr;

    // The section tree is single-threaded replay state — the whole test body re-runs once per section path, which only the test's own thread does.
    // So this is a framework misuse rather than something to serialize, and it is reported as one.
    if (!is_own_test_body(&ctx))
    {
        ctx.off_thread_failed_checks.fetch_add(1, cc::memory_order_relaxed);
        ctx.off_thread_errors.lock(
            [&](cc::vector<test_error>& errors)
            {
                errors.push_back(test_error{
                    .expr = cc::format("SECTION \"{}\" outside the test's own thread", name),
                    .location = location,
                    .extra_lines = {"sections are replayed by the test body, so only the test's own thread may open "
                                    "one"},
                    .expanded = cc::format("SECTION \"{}\" outside the test's own thread", name),
                });
            });
        return raii_section_opener(false);
    }

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
        auto& ctx = *current_context();
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
    // Nesting is not supported: a flat pointer keeps the hot path cheap, and the fuzz engine never nests captures.
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
    // The ambient chain, not this thread's stack: it holds the same enclosing tests, and holds them for work running anywhere.
    for (auto const* l = static_cast<cc::async_ambient_link const*>(cc::async_current_ambient()); l != nullptr;
         l = l->parent)
    {
        if (l->tag != test_ambient_tag())
            continue;
        auto const* const ctx = static_cast<test_context const*>(l->value);
        if (ctx != nullptr && ctx->execution != nullptr && ctx->execution->instance.declaration == decl)
            return true;
    }
    return false;
}

void nx::impl::report_invocation_cycle(nx::test_declaration const* decl)
{
    auto* const ctx = current_context();
    CC_ASSERT(ctx != nullptr, "must be called within a running test");
    auto const name = decl != nullptr ? cc::string_view(decl->name) : cc::string_view("<null>");
    ctx->errors.push_back(nx::test_error{
        .expr = cc::format("nx::invoke_tests cycle: \"{}\" is already running", name),
        .location = decl != nullptr ? decl->location : cc::source_location::current(),
        .extra_lines = {"an invocable must not (transitively) invoke itself"},
        .expanded = cc::format("invocation cycle: \"{}\" would recurse into itself", name),
    });
}

nx::test_execution* nx::impl::current_execution()
{
    auto const* const ctx = current_context();
    if (ctx == nullptr)
        return nullptr;
    return ctx->execution;
}

nx::test_schedule_config const* nx::impl::current_config()
{
    auto const* const ctx = current_context();
    if (ctx == nullptr)
        return nullptr;
    return ctx->config;
}

int nx::impl::current_filter_consumed()
{
    auto const* const ctx = current_context();
    if (ctx == nullptr)
        return 0;
    // curr_section always holds at least the root (which carries no filterable name)
    return ctx->filter_offset + int(ctx->curr_section.size()) - 1;
}

cc::span<cc::vector<cc::string> const> nx::impl::current_section_scopes()
{
    auto const* const ctx = current_context();
    if (ctx == nullptr)
        return {};
    return ctx->section_scopes;
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
    auto* const ctx = current_context();
    if (ctx == nullptr)
        return; // no active test — recording is a no-op outside a test body

    auto* const execution = ctx->execution;
    if (execution == nullptr)
        return;

    auto metric = nx::recorded_metric{cc::string(name), value, cc::string(unit), higher_is_better};
    if (is_own_test_body(ctx))
        execution->metrics.push_back(cc::move(metric));
    else
        ctx->off_thread_metrics.lock([&](cc::vector<nx::recorded_metric>& out) { out.push_back(cc::move(metric)); });
}

void nx::impl::report_check_result(check_result result)
{
    // Capture mode: a tool such as the fuzz engine is driving user code that is expected to fail often.
    // Tally the outcome and suppress both the host-test side effects and the control-flow throws (REQUIRE/SKIP).
    // One failing operation then neither aborts nor pollutes the host test.
    if (g_check_capture != nullptr)
    {
        auto& sink = *g_check_capture;
        ++sink.executed;
        if (result.op == cmp_op::skip)
            return;
        if (!result.passed)
        {
            ++sink.failed;
            if (result.kind == check_kind::require || result.op == cmp_op::assert_fail)
                sink.require_failed = true;
            if (sink.first_message.empty())
                sink.first_message = cc::format("{} | {}", result.expr, render_expanded(result));
        }
        return;
    }

    // An unattributable check fails the RUN — see g_orphan_checks for why silence is not an option here.
    auto* const ctx_ptr = current_context();
    if (ctx_ptr == nullptr)
    {
        report_orphan_check(cc::move(result), "no running test — start the thread with nx::attributed_to_current_test, "
                                              "or check on the test's own thread");
        return;
    }

    auto& ctx = *ctx_ptr;
    if (ctx.is_finished.load(cc::memory_order_acquire))
    {
        report_orphan_check(cc::move(result), cc::format("\"{}\" had already finished — its async work outlived it",
                                                         ctx.execution->instance.declaration->name));
        return;
    }

    // Reported from somewhere other than the test's own body: a pool worker driving its nodes, or a thread it started.
    // Counted on the side, and never allowed near the section tree.
    if (!is_own_test_body(&ctx))
    {
        report_off_thread_check_result(ctx, cc::move(result));
        return;
    }

    // Increment executed checks
    ++ctx.executed_checks;

    // If this is a SKIP, throw to abort test execution (counts as success)
    if (result.op == cmp_op::skip)
        throw test_skipped{};

    // If the check failed, record it
    if (!result.passed)
    {
        ++ctx.failed_checks;

        auto expanded = render_expanded(result);

        // Add test error
        ctx.errors.push_back(test_error{
            .expr = std::move(result.expr),
            .location = result.location,
            .extra_lines = std::move(result.extra_lines),
            .expanded = std::move(expanded),
        });

        // If this was a REQUIRE, throw exception to abort test execution
        if (result.kind == check_kind::require)
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
    auto* const ctx_ptr = test_execute_begin(execution, config, section_scopes, filter_offset);
    auto& ctx = *ctx_ptr;

    // Installing the context as the ambient is what makes a check find this test, from this thread or any other.
    // The scope closes before test_execute_end, so nothing can look the context up while it is being destroyed.
    auto leaked_async_work = false;
    {
        cc::async_ambient_scope const test_ambient(nx::impl::test_ambient_tag(), ctx_ptr);

        // Execute the test body, re-running it once per section-exploration pass
        auto section_num = 0;
        auto should_continue = true;
        while (should_continue)
        {
            // CAUTION: a test is allowed to run nested tests, thus growing the context stack here
            ctx.exec_count++;
            ctx.leaf_section = nullptr;
            ctx.root_section->next_open_section = nullptr;

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
                        nx::impl::report_check_result({
                            .kind = impl::check_kind::require,
                            .op = impl::cmp_op::assert_fail,
                            .expr = info.expression,
                            .passed = false,
                            .diagnostic = info.message,
                            .location = info.location,
                        });
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
                ctx.errors.push_back(test_error{
                    .expr = cc::format("duplicate section: \"{}\"", e.name),
                    .location = e.location,
                    .extra_lines = {},
                    .expanded = cc::format("duplicate section: \"{}\"", e.name),
                });
                should_continue = false; // wrong use of test framework
            }
            catch (std::exception const& e)
            {
                ctx.errors.push_back(test_error{
                    .expr = cc::format("uncaught exception: {}", e.what()),
                    .location = decl.location,
                    .extra_lines = {},
                    .expanded = cc::format("uncaught exception: {}", e.what()),
                });
            }
            catch (...)
            {
                ctx.errors.push_back(test_error{
                    .expr = "uncaught unknown exception",
                    .location = decl.location,
                    .extra_lines = {},
                    .expanded = "uncaught unknown exception",
                });
            }

            // associate stats & errors with leaf
            auto sec = ctx.leaf_section;
            if (sec == nullptr)
                sec = ctx.root_section.get();
            CC_ASSERT(sec != nullptr, "should always have a leaf section");
            {
                auto const t_section_end = std::chrono::high_resolution_clock::now();
                sec->duration_seconds = std::chrono::duration<double>(t_section_end - t_section_start).count();
                sec->executed_checks = cc::exchange(ctx.executed_checks, 0);
                sec->failed_checks = cc::exchange(ctx.failed_checks, 0);
                sec->errors = cc::exchange(ctx.errors, {});
            }

            // no new sections to execute? we're done
            if (ctx.root_section->next_open_section == nullptr)
            {
                // so it's not marked as unreachable
                ctx.root_section->is_done = true;

                // .. and we're done!
                should_continue = false;
            }
        }

        // Async work still carrying this test's context after its body is done is interference, not cleanup someone else will do:
        // it will run during a LATER test and report there.
        // Reported as a failure naming this test, which is the policy cc deliberately leaves to us.
        if (test_ambient.outstanding() != 0)
        {
            leaked_async_work = true;
            ctx.off_thread_failed_checks.fetch_add(1, cc::memory_order_relaxed);
            ctx.off_thread_errors.lock(
                [&](cc::vector<test_error>& errors)
                {
                    errors.push_back(test_error{
                        .expr = cc::format("\"{}\" left async work running", decl.name),
                        .location = decl.location,
                        .extra_lines = {"the work carries this test's context and would report into the next test's "
                                        "run — await or cancel it"},
                        .expanded
                        = cc::format("{} async item(s) still carry this test's context", test_ambient.outstanding()),
                    });
                });
        }
    }

    // Clean up test context (finalizes execution.root)
    test_execute_end(leaked_async_work);

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

    drain_orphan_checks(result);
    return result;
}
