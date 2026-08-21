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
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_ambient.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>
#include <clean-core/thread/thread_pump.hh>
#include <nexus/async-test.hh> // the submit_test_async seam an ASYNC_TEST body reaches us through
#include <nexus/fwd.hh>        // also what puts the bare sized aliases in scope inside nx
#include <nexus/tests/check.hh>
#include <nexus/tests/impl/test_ambient.hh>
#include <nexus/tests/section.hh>

#include <chrono>        // std::chrono: no cc timing yet
#include <cstdio>        // std::fputs / std::fwrite: crash-context hook writes to stderr without allocating
#include <string>        // std::string: key type for the std::unordered_map below
#include <unordered_map> // std::unordered_map: cc::map is not implemented yet

using namespace cc::primitive_defines;


/// The graph an ASYNC_TEST body hands back.
/// Opaque to every header, so only this file ever names an async type on the declaration path.
struct nx::impl::async_test_sink
{
    cc::shared_async<cc::unit> root;
};

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

    // False for an ASYNC_TEST: the section tree is replay state, and the body of an async test runs exactly once.
    bool allows_sections = true;

    // Where --verbose trace lines go: the top-level execution's buffer, shared with every context nested under it.
    // Never null while a body runs.
    cc::string* verbose_sink = nullptr;

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

// Contexts whose BODY this thread is currently inside, innermost last.
// Non-owning: ownership belongs to whoever runs the body, because a body that can suspend outlives any single stack frame.
// Entered once per body poll rather than once per test, so a body resuming on another thread is still recognized as its own there.
//
// This stack answers only "is this MY body".
// LOOKUP of which test a check belongs to goes through the ambient chain (current_context) instead.
// impl/test_ambient.hh has why a stack read is the wrong answer once work runs off the test's own thread.
thread_local cc::vector<test_context*> g_body_stack;

/// Marks the calling thread as running `ctx`'s body for as long as it lives.
struct scoped_body
{
    explicit scoped_body(test_context* ctx) { g_body_stack.push_back(ctx); }
    ~scoped_body() { g_body_stack.remove_back(); }

    scoped_body(scoped_body const&) = delete;
    scoped_body& operator=(scoped_body const&) = delete;
};

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
/// Being the innermost body this thread is inside is exactly the condition that excludes them.
bool is_own_test_body(test_context const* ctx)
{
    return !g_body_stack.empty() && g_body_stack.back() == ctx;
}

// What each thread is running right now: read by the crash-context hook report_running_test, and by a failing check to name what it ran beside.
//
// One slot per thread rather than one global: with tests in parallel a single slot names whichever test wrote last, which is exactly the wrong one to blame.
// A crash context may not allocate and may not lock, so the slots are a fixed array, claimed once per thread and never freed.
// A thread past the slot count is simply not reported — losing a name in a crash report beats growing a table inside one.
//
// The slot holds the DECLARATION rather than a (pointer, length) pair, because the check reader runs while other threads are still writing.
// One word cannot tear, and a declaration's name outlives the run, so a racing reader sees the previous test or the next one — never a pointer with the wrong length.
constexpr int max_running_test_slots = 64;

struct running_test_slot
{
    cc::atomic<nx::test_declaration const*> declaration = {nullptr};
    cc::atomic<int> section = {0};
};

running_test_slot g_running_tests[max_running_test_slots];
cc::atomic<int> g_running_slots_claimed = {0};
thread_local int g_running_slot = -1;

/// This thread's crash-context slot, or null once the table is full.
running_test_slot* running_test_slot_for_this_thread()
{
    if (g_running_slot < 0)
        g_running_slot = g_running_slots_claimed.fetch_add(
            1, cc::memory_order_relaxed); // may land past the end, which is the "no slot" answer below
    if (g_running_slot >= max_running_test_slots)
        return nullptr;
    return &g_running_tests[g_running_slot];
}

/// Publishes what this thread is running, restoring the enclosing test's entry on the way out.
/// Restoring is what keeps a driver named after one of its dispatched children finishes.
struct scoped_running_test
{
    explicit scoped_running_test(running_test_slot* slot) : _slot(slot)
    {
        if (_slot != nullptr)
        {
            _saved_declaration = _slot->declaration.load(cc::memory_order_relaxed);
            _saved_section = _slot->section.load(cc::memory_order_relaxed);
        }
    }
    ~scoped_running_test()
    {
        if (_slot != nullptr)
        {
            _slot->section.store(_saved_section, cc::memory_order_relaxed);
            _slot->declaration.store(_saved_declaration, cc::memory_order_relaxed);
        }
    }

    scoped_running_test(scoped_running_test const&) = delete;
    scoped_running_test& operator=(scoped_running_test const&) = delete;

private:
    running_test_slot* _slot = nullptr;
    nx::test_declaration const* _saved_declaration = nullptr;
    int _saved_section = 0;
};

/// Publishes `decl` (and its section index) as what this thread is running.
void publish_running_test(running_test_slot* slot, nx::test_declaration const& decl, int section)
{
    if (slot == nullptr)
        return;
    slot->section.store(section, cc::memory_order_relaxed);
    slot->declaration.store(&decl, cc::memory_order_relaxed);
}

/// The tests running on OTHER threads right now, as one annotation line, or empty when this test is alone.
///
/// A snapshot rather than a fact: a slot may change while this walks the table, so a name here means "was running around now".
/// That is the right resolution for the question it answers — which pair collided — and no lock could do better without changing what it measures.
cc::string other_running_tests()
{
    auto const claimed = cc::min(g_running_slots_claimed.load(cc::memory_order_relaxed), max_running_test_slots);

    cc::string line;
    for (auto i = 0; i < claimed; ++i)
    {
        if (i == g_running_slot)
            continue; // this thread's own slot names the failing test itself

        auto const* const decl = g_running_tests[i].declaration.load(cc::memory_order_relaxed);
        if (decl == nullptr || decl->name.empty())
            continue;

        line += line.empty() ? "also running: \"" : ", \"";
        line += decl->name;
        line += '"';
    }
    return line;
}

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

/// Build a context for one execution — the caller owns it, and installs it as the ambient, which is what makes it findable.
cc::unique_ptr<test_context> test_execute_begin(nx::test_execution& execution,
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

    // The ambient here is still the DISPATCHING test's context — this one is installed by the caller, just after us.
    // So a nested execution inherits its ancestor's sink, and only a top-level one owns a buffer.
    auto const* const parent = current_context();
    ctx.verbose_sink = parent != nullptr ? parent->verbose_sink : &execution.verbose_output;

    return owned;
}

/// Finalize `owned` into its execution and drop it.
/// The ambient scope naming this context is already gone by the time we run, precisely so nothing can look it up while it is being destroyed.
///
/// `keep_alive` is set when async work outlived the test and still carries a link naming this context.
/// It is then kept rather than destroyed, so that work reports an orphan instead of writing into freed memory.
void test_execute_end(cc::unique_ptr<test_context> owned, bool keep_alive)
{
    CC_ASSERT(owned != nullptr, "should always have a context to finalize");

    auto& ctx = *owned;
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

/// A failing CC_ASSERT reports exactly as a failing REQUIRE does.
/// Attribution is the ambient context's, not the thread's, so this is correct on a pool worker as well as on the test's own thread.
void report_assert_as_check(cc::impl::assertion_info const& info)
{
    nx::impl::report_check_result({
        .kind = impl::check_kind::require,
        .op = impl::cmp_op::assert_fail,
        .expr = info.expression,
        .passed = false,
        .diagnostic = info.message,
        .location = info.location,
    });
}

/// Install that handler on the thread running a test body.
auto scoped_test_assertion_handler()
{
    return cc::impl::scoped_assertion_handler(&report_assert_as_check);
}

/// Run one scheduled instance's body to completion, resolving its effective section scopes first.
///
/// Effective scopes: the instance's own grouped set (alias-expanded), else the run-global -c path presented as a single scope, else none (run everything).
/// The storage is built here rather than at schedule time, so it lives exactly as long as the body call it feeds.
void run_scheduled_instance(nx::test_execution& execution, nx::test_schedule_config const& config)
{
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
        execution, config, [&] { execution.instance.declaration->function(); }, section_scopes,
        /*filter_offset=*/0);
}

/// Record that a test ended with async work still carrying its context.
///
/// That work will run during a LATER test and report there, so it is interference rather than cleanup someone else will do.
/// Reported as a failure naming this test, which is the policy cc deliberately leaves to us.
void note_leaked_async_work(test_context& ctx, nx::test_declaration const& decl, i32 outstanding)
{
    ctx.off_thread_failed_checks.fetch_add(1, cc::memory_order_relaxed);
    ctx.off_thread_errors.lock(
        [&](cc::vector<test_error>& errors)
        {
            errors.push_back(test_error{
                .expr = cc::format("\"{}\" left async work running", decl.name),
                .location = decl.location,
                .extra_lines = {"the work carries this test's context and would report into the next test's "
                                "run — await or cancel it"},
                .expanded = cc::format("{} async item(s) still carry this test's context", outstanding),
            });
        });
}

/// What one ASYNC_TEST carries across the polls of its wrapper node.
struct async_test_state
{
    nx::test_execution* execution = nullptr;
    nx::test_schedule_config const* config = nullptr;

    cc::unique_ptr<test_context> ctx;

    // The ONE ambient link naming this test, made in the first poll and kept alive here for the rest of the node's life.
    // One link, not one per poll, because the leak check counts what still holds it.
    cc::async_ambient_handle ambient;

    cc::shared_async<cc::unit> root;
    std::chrono::high_resolution_clock::time_point started_at;
    bool started = false;
};

/// Run an ASYNC_TEST body to its return under `ctx`, and take the graph it handed back.
/// Null if the body threw before producing one.
cc::shared_async<cc::unit> run_async_prologue(test_context& ctx, nx::test_declaration const& decl)
{
    auto* const crash_slot = running_test_slot_for_this_thread();
    scoped_running_test const published(crash_slot);
    publish_running_test(crash_slot, decl, 0);

    scoped_body const own_body(&ctx);

    // Unbound exactly as a synchronous body is: whatever the body schedules for itself stays its own business.
    // Only the root it RETURNS is handed to the run's scheduler, and that happens after this returns.
    cc::async_no_worker_scope const unbound;

    nx::impl::async_test_sink sink;
    try
    {
        auto _ = scoped_test_assertion_handler();
        decl.async_function(sink);
    }
    catch (test_require_failed const&) // NOLINT(bugprone-empty-catch)
    {
        // already recorded by report_check_result; the throw only aborts the body
    }
    catch (test_skipped const&) // NOLINT(bugprone-empty-catch)
    {
        // already counted as a passing check; the throw only aborts the body
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
    return cc::move(sink.root);
}

/// Fold an async test's outcome into its execution and drop its context.
void finish_async_test(async_test_state& state)
{
    auto& ctx = *state.ctx;
    auto const& decl = *state.execution->instance.declaration;

    // The graph's failure channel is a TEST failure, never an error we pass on — see execute_tests on why a test node must resolve to a value.
    if (state.root != nullptr)
    {
        if (auto const* const err = state.root->try_error(); err != nullptr)
        {
            auto const what = err->is_cancelled() ? cc::string("cancelled") : err->underlying().to_string();
            ctx.errors.push_back(test_error{
                .expr = cc::format("the test's async graph failed: {}", what),
                .location = decl.location,
                .extra_lines = {},
                .expanded = cc::format("async graph resolved to an error: {}", what),
            });
        }
    }

    // No section replay here, so everything the body's own thread reported belongs to the root section.
    auto& sec = *ctx.root_section;
    sec.duration_seconds
        = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - state.started_at).count();
    sec.executed_checks += cc::exchange(ctx.executed_checks, 0);
    sec.failed_checks += cc::exchange(ctx.failed_checks, 0);
    sec.errors.push_back_range(cc::exchange(ctx.errors, {}));

    // Drop the root before counting: a ready node carries no context, but whatever it left behind still does.
    state.root = {};

    // `ambient` holds exactly one reference; anything beyond it is work that outlived the test still naming it.
    auto const* const link = static_cast<cc::async_ambient_link const*>(state.ambient.head());
    auto const outstanding = link != nullptr ? link->refs.load(cc::memory_order_acquire) - 1 : 0;
    auto const leaked = outstanding > 0;
    if (leaked)
        note_leaked_async_work(ctx, decl, outstanding);

    test_execute_end(cc::move(state.ctx), leaked);
}

/// Drive one poll of an async test's wrapper node.
/// Waiting while the body's graph is still running; resolved once the test has been finalized.
cc::async_step_status step_async_test(async_test_state& state, cc::async_context<cc::unit>& actx)
{
    if (!state.started)
    {
        state.started = true;
        state.started_at = std::chrono::high_resolution_clock::now();
        state.ctx = test_execute_begin(*state.execution, *state.config, {}, /*filter_offset=*/0);
        state.ctx->allows_sections = false;

        auto const& decl = *state.execution->instance.declaration;

        // The link this test is known by.
        // Pushed and popped inside this one poll, which is the only shape async_ambient_scope allows, and kept alive past it by `ambient`.
        cc::async_ambient_scope const scope(nx::impl::test_ambient_tag(), state.ctx.get());
        state.ambient = cc::async_ambient_handle();

        state.root = run_async_prologue(*state.ctx, decl);

        // Scheduling a COLD node stamps the calling thread's ambient onto it as its resume token — this scope.
        // That single stamp is what makes every check the graph reports find this test, from whichever worker polls it,
        // and it also reaches the cold nodes the graph drives inline, since those inherit their driver's context.
        if (state.root != nullptr)
        {
            CC_ASSERT(state.root->is_cold(), "an ASYNC_TEST must return a cold graph — see nexus/async-test.hh");
            state.root->schedule();
        }
    }

    if (state.root != nullptr && !actx.require(state.root))
        return actx.wait_for_dependencies();

    finish_async_test(state);
    return actx.resolve_to_value(cc::unit{}); // terminal: nothing may follow it
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
    CC_ASSERT(ctx.allows_sections, "SECTION is not available in an ASYNC_TEST: the section tree is replay state, and "
                                   "an async body runs once");

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

void nx::impl::submit_test_async(async_test_sink& sink, cc::shared_async<cc::unit> root)
{
    CC_ASSERT(root != nullptr, "an ASYNC_TEST body must return a valid async");
    CC_ASSERT(sink.root == nullptr, "an ASYNC_TEST body must hand back exactly one graph");
    sink.root = cc::move(root);
}

nx::test_registry const* nx::impl::active_registry()
{
    auto const* const ctx = current_context();
    if (ctx == nullptr || ctx->execution == nullptr)
        return nullptr;
    return ctx->execution->instance.registry;
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
    // Every running test, not just this thread's: under -jN the faulting thread is often not the interesting one.
    auto const claimed = cc::min(g_running_slots_claimed.load(cc::memory_order_relaxed), max_running_test_slots);
    auto reported = 0;
    for (auto i = 0; i < claimed; ++i)
    {
        auto const* const decl = g_running_tests[i].declaration.load(cc::memory_order_relaxed);
        if (decl == nullptr || decl->name.empty())
            continue;

        std::fputs(reported == 0 ? "running test: \"" : "   also running: \"", stderr);
        std::fwrite(decl->name.data(), 1, size_t(decl->name.size()), stderr);
        std::fputc('"', stderr);
        if (auto const section = g_running_tests[i].section.load(cc::memory_order_relaxed); section > 0)
            std::fprintf(stderr, " (section %d)", section);
        std::fputc('\n', stderr);
        ++reported;
    }

    if (reported == 0)
        std::fputs("running test: <none>\n", stderr);
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

    // A failure at -jN is often about what it ran BESIDE, and the report otherwise names only the test that failed.
    // The crash hook already knows the answer; a failing check is the other place that needs it.
    if (!result.passed && result.op != cmp_op::skip)
        if (auto beside = other_running_tests(); !beside.empty())
            result.extra_lines.push_back(cc::move(beside));

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
    auto owned_ctx = test_execute_begin(execution, config, section_scopes, filter_offset);
    auto& ctx = *owned_ctx;

    // Installing the context as the ambient is what makes a check find this test, from this thread or any other.
    // The scope closes before test_execute_end, so nothing can look the context up while it is being destroyed.
    auto leaked_async_work = false;
    {
        cc::async_ambient_scope const test_ambient(nx::impl::test_ambient_tag(), &ctx);
        scoped_body const own_body(&ctx); // this thread is inside ctx's body from here until the replay loop ends

        auto* const crash_slot = running_test_slot_for_this_thread();
        scoped_running_test const published(crash_slot);

        // Execute the test body, re-running it once per section-exploration pass
        auto section_num = 0;
        auto should_continue = true;
        while (should_continue)
        {
            // CAUTION: a test is allowed to run nested tests, thus growing the body stack here
            ctx.exec_count++;
            ctx.leaf_section = nullptr;
            ctx.root_section->next_open_section = nullptr;

            // publish the running test for the crash-context hook (points a fatal fault at this test)
            publish_running_test(crash_slot, decl, section_num);

            if (config.verbose)
            {
                if (section_num == 0)
                    *ctx.verbose_sink += cc::format("  - start \"{}\"\n", decl.name);
                else
                    *ctx.verbose_sink += cc::format("  - start \"{}\" section {}\n", decl.name, section_num);
            }
            section_num++;
            auto const t_section_start = std::chrono::high_resolution_clock::now();

            try
            {
                auto _ = scoped_test_assertion_handler(); // a failing CC_ASSERT aborts the body like a REQUIRE
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

        if (test_ambient.outstanding() != 0)
        {
            leaked_async_work = true;
            note_leaked_async_work(ctx, decl, test_ambient.outstanding());
        }
    }

    // test_execute_end drops (or parks) the context, so take the sink while it is still ours to read.
    auto* const verbose_sink = ctx.verbose_sink;

    // Clean up test context (finalizes execution.root)
    test_execute_end(cc::move(owned_ctx), leaked_async_work);

    if (config.verbose)
    {
        double const duration_ms = execution.root.duration_seconds * 1000.0;
        *verbose_sink
            += cc::format("    ... in {:.2f} ms ({} checks, {} failed checks, {} errors)\n", duration_ms,
                          execution.root.executed_checks, execution.root.failed_checks, execution.root.errors.size());
    }
}

namespace
{
/// Swaps the process-wide ambient scheduler for one phase, restoring the run's own afterwards.
/// A null `next` is the phase that wants none at all.
struct scoped_ambient_override
{
    explicit scoped_ambient_override(cc::async_scheduler* next) : _previous(cc::async_scheduler::default_or_null())
    {
        if (_previous != nullptr)
            cc::uninstall_default_async_scheduler(*_previous);
        if (next != nullptr)
            cc::install_default_async_scheduler(*next);
        _installed = next;
    }

    ~scoped_ambient_override()
    {
        if (_installed != nullptr)
            cc::uninstall_default_async_scheduler(*_installed);
        if (_previous != nullptr)
            cc::install_default_async_scheduler(*_previous);
    }

    scoped_ambient_override(scoped_ambient_override const&) = delete;
    scoped_ambient_override& operator=(scoped_ambient_override const&) = delete;

private:
    cc::async_scheduler* _previous = nullptr;
    cc::async_scheduler* _installed = nullptr;
};
} // namespace

nx::test_schedule_execution nx::execute_tests(test_schedule const& schedule, test_schedule_config const& config)
{
    test_schedule_execution result;

    if (config.verbose)
    {
        cc::println("executing {} tests", schedule.instances.size());
    }

    // A run stands up its own scheduler, and schedulers do not nest: one bound here would be an st scheduler inside whatever is already driving.
    // So a test that runs its own execute_tests — nexus' own meta-tests — must ask for nx::no_scheduler.
    CC_ASSERT(cc::async_scheduler::current_or_null() == nullptr, "execute_tests may not run under a scheduler; a test "
                                                                 "that nests a run must declare nx::no_scheduler");

    // Validated BEFORE the fallback handler below goes up.
    // Past that point report_assert_as_check records a check and RETURNS, so a CC_ASSERT here would become an orphan check and the run would continue —
    // which for a misplaced main_thread test means falling straight into the abort the flag exists to prevent.
    auto any_main_thread = false;
    for (auto const& instance : schedule.instances)
    {
        CC_ASSERT(instance.declaration != nullptr, "instances must be valid");
        if (!instance.declaration->test_config.main_thread)
            continue;

        any_main_thread = true;
        CC_ASSERT(instance.declaration->test_config.scheduler != nx::config::scheduler_mode::own_pool,
                  "nx::main_thread cannot be combined with own_pool: a private pool's worker is never the main thread");
        CC_ASSERT(!instance.declaration->is_async(), "an ASYNC_TEST cannot use nx::main_thread: the graph it returns "
                                                     "is driven by the phase's scheduler, not by the thread the body "
                                                     "started on");
    }
    CC_ASSERT(!any_main_thread || cc::current_thread_id() == cc::thread_id::main,
              "a test asked for nx::main_thread, but execute_tests is not running on the main thread; a binary running "
              "tests without nx::run must call cc::mark_current_thread_as_main() from main()");

    // The per-body handler covers the thread the body runs on; this covers every OTHER thread the run reaches — pool workers driving an ASYNC_TEST's graph, and threads a test started itself.
    // Without it a CC_ASSERT there aborts the process instead of failing a test.
    cc::impl::scoped_fallback_assertion_handler const assert_fallback(&report_assert_as_check);

    // Pre-sized and written by index, never appended to.
    // Report order is then the SCHEDULE's, whatever order the tests actually ran in, and the slot a running test writes into cannot be reallocated out from under it.
    result.executions.resize_to_defaulted(schedule.instances.size());

    // Partition by scheduler mode, in first-appearance order.
    // Each partition is one graph under one scheduler, run as its own phase — schedulers do not nest, so they cannot overlap.
    // Phases being sequential is also what makes exclusivity ACROSS modes free: only within-phase pairs need an edge.
    struct run_phase
    {
        nx::config::scheduler_mode mode = nx::config::scheduler_mode::shared;
        nx::config::ambient_mode ambient = nx::config::ambient_mode::multi_threaded;
        int threads = 0;
        cc::vector<isize> indices;
    };
    cc::vector<run_phase> phases;

    for (isize i = 0; i < schedule.instances.size(); ++i)
    {
        auto const& instance = schedule.instances[i];
        CC_ASSERT(instance.declaration != nullptr, "instances must be valid");
        CC_ASSERT(instance.declaration->function.is_valid() || instance.declaration->is_async(),
                  "ordinary instances must have a nullary or an async body");
        CC_ASSERT(!instance.declaration->is_async()
                      || instance.declaration->test_config.scheduler != nx::config::scheduler_mode::none,
                  "an ASYNC_TEST cannot use no_scheduler: nothing would drive the graph it returns");
        auto& execution = result.executions[i];
        execution.instance = instance;
        if (execution.instance.registry == nullptr)
            execution.instance.registry = schedule.registry; // a hand-built schedule may only name the registry once

        // main_thread is orthogonal in the API and has exactly one implementation today: drive the body directly on the run's calling thread, which the pre-pass proved is the main one.
        // Keeping that mapping to ONE line is what lets a future main-thread-driven phase replace it without touching a single test's config.
        //
        // A no-arg exclusive() test runs beside nothing, so a node buys it nothing and costs a barrier's worth of edges plus a stalled pool.
        // The no-scheduler group already runs bodies one at a time on the calling thread, which is the same guarantee for free — so route it there.
        // Only a synchronous body under the run's own scheduler: an ASYNC_TEST needs a scheduler to drive the root it returns, and own_pool was asked for on purpose.
        // The cost is that such a test no longer orders against the shared phase, only within the no-scheduler one.
        auto mode = instance.declaration->test_config.scheduler;
        if (instance.declaration->test_config.main_thread)
            mode = nx::config::scheduler_mode::none;
        else if (mode == nx::config::scheduler_mode::shared && instance.declaration->test_config.exclusive_global
                 && !instance.declaration->is_async())
            mode = nx::config::scheduler_mode::none;

        // The ambient scheduler is the phase's too: it is installed process-wide for the phase, so tests wanting different ones cannot share it.
        // Only a directly driven body may ask for anything but a pool — a body running as a node on one already has it bound.
        auto const ambient = instance.declaration->test_config.ambient;
        CC_ASSERT(mode == nx::config::scheduler_mode::none || ambient == nx::config::ambient_mode::multi_threaded,
                  "nx::no_scheduler and nx::singlethreaded drive the body directly, so they cannot be combined with a "
                  "scheduler mode that runs it as a node");

        auto const threads = instance.declaration->test_config.scheduler_threads;
        auto* phase = static_cast<run_phase*>(nullptr);
        for (auto& p : phases)
            if (p.mode == mode && p.ambient == ambient && p.threads == threads)
            {
                phase = &p;
                break;
            }
        if (phase == nullptr)
        {
            phases.push_back(run_phase{.mode = mode, .ambient = ambient, .threads = threads, .indices = {}});
            phase = &phases.back();
        }
        phase->indices.push_back(i);
    }

    // ONE ambient scheduler for the whole run, and deliberately not one per phase.
    // Work a test left running outlives its phase — an actor thread completing a node is the usual shape — and a completion with nothing installed has nowhere to route.
    // It is never the scheduler driving the tests either, so a body that blocks on its own graph can never end up running another test's.
    cc::async_thread_pool run_ambient(config.jobs > 0 ? cc::max(config.jobs - 1, 1)
                                                      : cc::async_thread_pool::default_worker_count());
    cc::scoped_default_async_scheduler const run_ambient_installed(run_ambient);

    for (auto const& phase : phases)
    {
        // Bodies driven directly: no graph, since with nothing to schedule a node would only wrap a call.
        if (phase.mode == nx::config::scheduler_mode::none)
        {
            auto const run_bodies = [&]
            {
                for (auto const i : phase.indices)
                    run_scheduled_instance(result.executions[i], config);
            };

            switch (phase.ambient)
            {
            case nx::config::ambient_mode::multi_threaded:
                run_bodies(); // the run's own, which is what every other phase gets too
                break;

            case nx::config::ambient_mode::none:
            {
                // The one mode that leaves the thread unbound and installs nothing, so a test nesting its own run finds nothing above it.
                scoped_ambient_override const overridden(nullptr);
                run_bodies();
                break;
            }

            case nx::config::ambient_mode::single_threaded:
            {
                // Bound as well as installed: binding is what makes every graph run inline on this thread, in order, which is what the mode is for.
                cc::singlethreaded_scheduler scheduler;
                cc::async_worker_scope const scope(scheduler);
                scoped_ambient_override const overridden(&scheduler);
                run_bodies();
                break;
            }
            }
            continue;
        }

        // own_pool names its own width; everything else is capped by the run's --jobs, where 0 means the machine's hardware concurrency.
        // Resolved here rather than at argument parsing, so a hand-built config means the same thing as a command line.
        auto const jobs = phase.mode == nx::config::scheduler_mode::own_pool ? phase.threads
                        : config.jobs <= 0                                   ? cc::num_hardware_threads()
                                                                             : config.jobs;

        // One node per test — the graph IS the phase, and which thread picks up which test is the scheduler's business.
        // A test node ALWAYS resolves to a value, never to an error: exclusivity is a dependency edge between test nodes, so an error here would propagate into every test ordered behind this one.
        // run_test_body already contains everything a body can throw.
        //
        // Exclusion is an ORDERING EDGE, not a lock: a test requires the last holder of each tag it carries, and becomes that tag's new tail.
        // Every edge therefore points backwards in schedule order, so the result is a DAG by construction — no admission control, no deadlock, and no starvation to guard against.
        // The price is that holders of a tag run in schedule order rather than in any order, which is a reproducibility win.
        cc::vector<cc::shared_async<cc::unit>> test_nodes;
        test_nodes.reserve(phase.indices.size());

        struct tag_tail
        {
            cc::string_view tag;
            cc::shared_async<cc::unit> node;
        };
        cc::vector<tag_tail> tag_tails;

        // A no-arg exclusive() is a barrier: it follows everything before it, and everything after follows it.
        // Tracking the window since the last barrier keeps a barrier's edge count to the tests it actually has to wait for.
        cc::shared_async<cc::unit> last_barrier;
        cc::vector<cc::shared_async<cc::unit>> nodes_since_barrier;

        for (auto const i : phase.indices)
        {
            auto* const execution = &result.executions[i];
            auto const& test_config = execution->instance.declaration->test_config;
            CC_ASSERT(test_config.exclusion_tag_count <= nx::config::max_exclusion_tags,
                      "a test asked for more exclusion tags than nx::config::max_exclusion_tags holds");

            cc::vector<cc::shared_async<cc::unit>> predecessors;
            if (test_config.exclusive_global)
            {
                predecessors = nodes_since_barrier;
                if (predecessors.empty() && last_barrier != nullptr)
                    predecessors.push_back(last_barrier);
            }
            else
            {
                // Everything follows the last barrier, tagged or not — that is what "runs alone" means.
                if (last_barrier != nullptr)
                    predecessors.push_back(last_barrier);
                for (auto t = 0; t < test_config.exclusion_tag_count; ++t)
                {
                    auto const tag = cc::string_view(test_config.exclusion_tags[t]);
                    for (auto const& tail : tag_tails)
                        if (tail.tag == tag)
                        {
                            predecessors.push_back(tail.node);
                            break;
                        }
                }
            }

            auto node = cc::make_async_lazy<cc::unit>(
                [execution, &config, predecessors = cc::move(predecessors),
                 async_state = cc::unique_ptr<async_test_state>()](
                    cc::async_context<cc::unit>& actx) mutable -> cc::async_step_status
                {
                    if (!predecessors.empty())
                    {
                        auto all_ready = true;
                        for (auto const& p : predecessors)
                            all_ready = actx.require(p) && all_ready; // never short-circuit: every one must be registered
                        if (!all_ready)
                            return actx.wait_for_dependencies();
                        predecessors.clear(); // done with them, and a held handle pins its whole subgraph alive
                    }

                    // An async test suspends, so it needs state across polls and a finalize that runs on whichever poll finishes it.
                    if (execution->instance.declaration->is_async())
                    {
                        if (async_state == nullptr)
                            async_state = cc::make_unique<async_test_state>(
                                async_test_state{.execution = execution, .config = &config});
                        return step_async_test(*async_state, actx);
                    }

                    // The run's scheduler drives TESTS, never the work inside one.
                    // Left bound, a node the body schedules would land in our queue, and we would run it after the test — outside the lifetime of everything it captured.
                    // Unbound, a test sees exactly the thread it always saw: nothing above it, and its own graphs driven by its own scheduler.
                    {
                        cc::async_no_worker_scope const unbound;
                        run_scheduled_instance(*execution, config);
                    }
                    return actx.resolve_to_value(cc::unit{}); // terminal: nothing may follow it
                });

            if (test_config.exclusive_global)
            {
                last_barrier = node;
                nodes_since_barrier.clear();
                tag_tails.clear(); // every tag's tail is now the barrier, which everything after already follows
            }
            else
            {
                nodes_since_barrier.push_back(node);
                for (auto t = 0; t < test_config.exclusion_tag_count; ++t)
                {
                    auto const tag = cc::string_view(test_config.exclusion_tags[t]);
                    auto* existing = static_cast<tag_tail*>(nullptr);
                    for (auto& tail : tag_tails)
                        if (tail.tag == tag)
                        {
                            existing = &tail;
                            break;
                        }
                    if (existing != nullptr)
                        existing->node = node;
                    else
                        tag_tails.push_back(tag_tail{.tag = tag, .node = node});
                }
            }

            test_nodes.push_back(cc::move(node));
        }

        if (jobs <= 1)
        {
            // Serial: drive one node at a time, so the run order IS the schedule order.
            // Not a fan-out join with an st scheduler — a join picks whichever pending dependency it likes, and a chain of edges
            // would drive the whole schedule depth-first past the inline depth cap.
            //
            // The driver is bound but never ambient: its queue holds THE REMAINING TEST NODES, and a body that
            // blocked on it would run other tests, bodies and all, nested inside itself.
            cc::singlethreaded_scheduler driver;
            cc::async_worker_scope const scope(driver);
            for (auto const& node : test_nodes)
                (void)cc::async_blocking_get_on(driver, node);
            continue;
        }

        // Parallel: one join requiring every test node, driven on a pool.
        // Requires everything again on each poll rather than assuming a wake means all-ready — idempotent, and it costs one pass over a vector we already hold.
        auto const join = cc::make_async_lazy<cc::unit>(
            [nodes = cc::move(test_nodes)](cc::async_context<cc::unit>& actx) -> cc::async_step_status
            {
                auto all_ready = true;
                for (auto const& n : nodes)
                    all_ready = actx.require(n) && all_ready; // never short-circuit: every dependency must be registered
                if (!all_ready)
                    return actx.wait_for_dependencies();
                return actx.resolve_to_value(cc::unit{});
            });

        // One fewer worker than the job count: the thread driving here participates as one.
        // It is the phase's ambient scheduler too, so a body's own async work belongs to the pool already running it —
        // the run's own stays installed around the phase, for work that outlives it.
        cc::async_thread_pool pool(jobs - 1);
        scoped_ambient_override const overridden(&pool);
        (void)cc::async_blocking_get_on(pool, join);
    }

    // Flush the buffered per-test traces in schedule order, so --verbose reads the same however the tests ran.
    if (config.verbose)
    {
        for (auto const& execution : result.executions)
            cc::print(execution.verbose_output);
    }

    drain_orphan_checks(result);

    // A registered pump outliving the run means a semantic thread was never torn down.
    // Nothing fails yet — the run's own result is already computed — but it is silent otherwise, and the next run
    // inherits a pump into freed memory.
    if (auto const leaked = cc::registered_thread_pump_count(); leaked > 0)
        cc::print("[nexus] warning: {} thread pump(s) still registered after the run - a semantic thread outlived it\n",
                  leaked);

    return result;
}
