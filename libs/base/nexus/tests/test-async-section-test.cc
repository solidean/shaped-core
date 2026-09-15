#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_ambient.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>
#include <nexus/tests/thread_scope.hh>

// SECTION in an ASYNC_TEST body.
//
// An async body is replayed once per section path exactly as a synchronous one is: every pass gets a fresh coroutine,
// driven inside the one test node.
// Meta-tests: each runs a local registry nested and reads the section tree back.

ASYNC_TEST("async section - a coroutine body opens sections and awaits inside them")
{
    auto const before = co_await cc::make_async_lazy([] { return 1; });
    SECTION("first")
    {
        co_await cc::async_yield();
        CHECK(before == 1);
    }
    SECTION("second")
    {
        CHECK(co_await cc::make_async_lazy([] { return 2; }) == 2);
    }
}

namespace
{
struct probe
{
    cc::atomic<int> setup = {0};
    cc::atomic<int> first = {0};
    cc::atomic<int> nested_a = {0};
    cc::atomic<int> nested_b = {0};
    cc::atomic<int> last = {0};

    // Held past a section to leak the test's context deterministically, the way an unjoined attributed thread would.
    cc::async_ambient_handle kept;

    // The handshake that forces two strands to interleave their sections, whatever order they are polled in.
    cc::shared_async<cc::unit> a_opened = cc::make_async_manual<cc::unit>();
    cc::shared_async<cc::unit> b_opened = cc::make_async_manual<cc::unit>();
    cc::shared_async<cc::unit> a_closed = cc::make_async_manual<cc::unit>();

    nx::invocation_result invoked;
};

using body_fn = cc::shared_async<cc::unit> (*)(probe*);

struct mode
{
    int jobs;
    nx::config::cfg cfg;
};

/// The drives an async body can get: serial, on a pool, inline on the run thread, and homed to main.
cc::vector<mode> all_modes()
{
    return {
        {.jobs = 1, .cfg = {}},
        {.jobs = 4, .cfg = {}},
        {.jobs = 4, .cfg = nx::impl::merge_config(nx::config::singlethreaded)},
        {.jobs = 4, .cfg = nx::impl::merge_config(nx::config::main_thread)},
    };
}

nx::test_schedule_execution run_body(body_fn body, probe& p, mode const& m, cc::vector<cc::string> section_filters = {})
{
    nx::test_registry reg;
    reg.add_async_declaration(
        "subject", m.cfg, [&p, body](nx::impl::async_test_sink& sink) { nx::impl::submit_test_async(sink, body(&p)); });

    auto config = nx::test_schedule_config{};
    config.jobs = m.jobs;
    config.section_filters = cc::move(section_filters);
    auto const schedule = nx::test_schedule::create({}, reg);
    return nx::execute_tests(schedule, config);
}

bool mentions(cc::span<nx::test_error const> errors, cc::string_view text)
{
    auto found = false;
    for (auto const& err : errors)
        found |= err.expr.contains(text) || err.expanded.contains(text);
    return found;
}

int count_mentions(cc::span<nx::test_error const> errors, cc::string_view text)
{
    auto n = 0;
    for (auto const& err : errors)
        n += err.expr.contains(text) ? 1 : 0;
    return n;
}

cc::shared_async<cc::unit> nested_tree(probe* p)
{
    p->setup.fetch_add(1);
    co_await cc::async_yield();

    SECTION("first")
    {
        co_await cc::async_yield();
        p->first.fetch_add(1);
        CHECK(true);
    }
    SECTION("outer")
    {
        co_await cc::async_yield();
        SECTION("a")
        {
            p->nested_a.fetch_add(1);
            co_await cc::async_yield();
            CHECK(true);
            CHECK(true);
        }
        SECTION("b")
        {
            co_await cc::async_yield();
            p->nested_b.fetch_add(1);
            CHECK(true);
        }
    }
}
} // namespace

TEST("async section - every leaf runs once, the setup once per pass, and each leaf keeps its own checks", no_scheduler)
{
    for (auto const& m : all_modes())
    {
        probe p;
        auto const exec = run_body(&nested_tree, p, m);

        REQUIRE(exec.executions.size() == 1);
        auto const& root = exec.executions[0].root;
        CHECK(!exec.executions[0].is_considered_failing());
        CHECK(p.setup.load() == 3); // one pass per leaf: first, outer/a, outer/b
        CHECK(p.first.load() == 1);
        CHECK(p.nested_a.load() == 1);
        CHECK(p.nested_b.load() == 1);

        // Filed under the leaf that ran them, although every one was reported off the test's thread.
        REQUIRE(root.subsections.size() == 2);
        CHECK(root.subsections[0].name == "first");
        CHECK(root.subsections[0].executed_checks == 1);
        auto const& outer = root.subsections[1];
        REQUIRE(outer.subsections.size() == 2);
        CHECK(outer.subsections[0].executed_checks == 2);
        CHECK(outer.subsections[1].executed_checks == 1);
        CHECK(root.executed_checks == 4);
    }
}

TEST("async section - a -c path selects one nested leaf of an async test", no_scheduler)
{
    probe p;
    auto const exec = run_body(&nested_tree, p, all_modes()[1], {"outer", "b"});

    CHECK(exec.count_failed_tests() == 0);
    CHECK(p.setup.load() == 1);
    CHECK(p.first.load() == 0);
    CHECK(p.nested_a.load() == 0);
    CHECK(p.nested_b.load() == 1);
}

namespace
{
cc::shared_async<cc::unit> require_in_last(probe* p)
{
    SECTION("passes")
    {
        co_await cc::async_yield();
        p->first.fetch_add(1);
        CHECK(true);
    }
    SECTION("requires")
    {
        co_await cc::async_yield();
        REQUIRE(1 == 2);
        p->last.fetch_add(1); // never reached
    }
}

cc::shared_async<int> fails_later()
{
    co_await cc::async_yield();
    co_return cc::error("deliberate");
}

cc::shared_async<cc::unit> dependency_fails_in_last(probe* p)
{
    SECTION("passes")
    {
        p->first.fetch_add(1);
        CHECK(true);
    }
    SECTION("awaits a failure")
    {
        CHECK(true);
        (void)co_await fails_later();
        p->last.fetch_add(1); // never reached
    }
}
} // namespace

TEST("async section - a failed REQUIRE ends its own pass and fails only its own leaf", no_scheduler)
{
    for (auto const& m : all_modes())
    {
        probe p;
        auto const exec = run_body(&require_in_last, p, m);

        REQUIRE(exec.executions.size() == 1);
        auto const& root = exec.executions[0].root;
        CHECK(exec.executions[0].is_considered_failing());
        CHECK(p.first.load() == 1);
        CHECK(p.last.load() == 0);

        REQUIRE(root.subsections.size() == 2);
        CHECK(!root.subsections[0].is_considered_failing);
        CHECK(root.subsections[1].is_considered_failing);
        CHECK(root.subsections[1].errors.size() == 1);
        CHECK(!mentions(root.errors, "async graph failed")); // the REQUIRE's own throw, not a second failure
    }
}

TEST("async section - an awaited dependency that fails is reported on the leaf that awaited it", no_scheduler)
{
    for (auto const& m : all_modes())
    {
        probe p;
        auto const exec = run_body(&dependency_fails_in_last, p, m);

        REQUIRE(exec.executions.size() == 1);
        auto const& root = exec.executions[0].root;
        CHECK(p.first.load() == 1);
        CHECK(p.last.load() == 0);

        REQUIRE(root.subsections.size() == 2);
        CHECK(!root.subsections[0].is_considered_failing);
        CHECK(mentions(root.subsections[1].errors, "deliberate"));
    }
}

namespace
{
cc::shared_async<cc::unit> duplicate_names(probe* p)
{
    SECTION("same")
    {
        co_await cc::async_yield();
        CHECK(true);
    }
    SECTION("same")
    {
        p->last.fetch_add(1);
        CHECK(true);
    }
}

cc::shared_async<cc::unit> leaks_in_first(probe* p)
{
    SECTION("leaks")
    {
        co_await cc::async_yield();
        p->kept = nx::capture_current_test(); // still carries this test once the pass is over
        CHECK(true);
    }
    SECTION("after")
    {
        p->last.fetch_add(1);
        CHECK(true);
    }
}

cc::shared_async<cc::unit> fails_everywhere(probe* p)
{
    SECTION("a")
    {
        co_await cc::async_yield();
        for (auto i = 0; i < 100; ++i)
        {
            CHECK(false);
            p->first.fetch_add(1);
        }
    }
    SECTION("b")
    {
        p->last.fetch_add(1);
        CHECK(false);
    }
}
} // namespace

TEST("async section - a duplicate section fails the test once and ends the replay", no_scheduler)
{
    for (auto const& m : all_modes())
    {
        probe p;
        auto const exec = run_body(&duplicate_names, p, m);

        REQUIRE(exec.executions.size() == 1);
        auto const& errors = exec.executions[0].root.errors;
        CHECK(exec.executions[0].is_considered_failing());
        CHECK(p.last.load() == 0);
        CHECK(count_mentions(errors, "duplicate section") == 1);
        CHECK(!mentions(errors, "async graph failed"));
    }
}

TEST("async section - work outliving a pass fails that leaf and ends the replay", no_scheduler)
{
    for (auto const& m : all_modes())
    {
        probe p;
        auto const exec = run_body(&leaks_in_first, p, m);
        p.kept.reset();

        REQUIRE(exec.executions.size() == 1);
        auto const& root = exec.executions[0].root;
        CHECK(exec.executions[0].is_considered_failing());
        CHECK(p.last.load() == 0); // the next pass would have received the leaked work's reports

        REQUIRE(root.subsections.size() == 2);
        CHECK(mentions(root.subsections[0].errors, "left async work running"));
    }
}

TEST("async section - the failure cap applies to an async body, across its sections", no_scheduler)
{
    for (auto const& m : all_modes())
    {
        probe p;
        auto const exec = run_body(&fails_everywhere, p, m);

        CHECK(exec.count_failed_tests() == 1);
        CHECK(exec.count_failed_checks() == 30);
        CHECK(p.first.load() == 29); // the thirtieth failure throws
        CHECK(p.last.load() == 0);   // and ends the test, not just its pass
        CHECK(count_mentions(exec.executions[0].root.errors, "too many failed checks") == 1);
    }
}

namespace
{
cc::shared_async<cc::unit> strand_a(probe* p)
{
    SECTION("a")
    {
        CHECK(true);
        p->a_opened->push_value({});
        co_await p->b_opened; // "b" is opened on top of "a" before this resumes
    } // closes "a" while "b" is still on top
    p->a_closed->push_value({});
}

cc::shared_async<cc::unit> strand_b(probe* p)
{
    co_await p->a_opened;
    SECTION("b")
    {
        CHECK(true);
        p->b_opened->push_value({});
        co_await p->a_closed;
    }
}

cc::shared_async<cc::unit> interleaving_strands(probe* p)
{
    auto const a = strand_a(p);
    auto const b = strand_b(p);
    co_await cc::async_all(a, b);
}
} // namespace

TEST("async section - sections from concurrent strands that close out of order fail the test by name", no_scheduler)
{
    for (auto const& m : all_modes())
    {
        probe p;
        auto const exec = run_body(&interleaving_strands, p, m);

        REQUIRE(exec.executions.size() == 1);
        auto const& errors = exec.executions[0].root.errors;
        CHECK(exec.executions[0].is_considered_failing());
        CHECK(count_mentions(errors, "opened by concurrent work") == 1);
        CHECK(!mentions(errors, "async graph failed"));
    }
}

namespace
{
// The join key: a struct holding the probe, since a typed_value refuses a raw pointer on its own.
struct probe_key
{
    probe* p = nullptr;
};

cc::shared_async<cc::unit> invocable_child(probe_key const& k)
{
    CHECK(k.p != nullptr);
    co_return;
}

cc::shared_async<cc::unit> invokes_inside_a_section(probe* p)
{
    SECTION("plain")
    {
        CHECK(true);
    }
    SECTION("dispatch")
    {
        p->invoked = co_await nx::async_invoke_tests_in_sequence("g", probe_key{p});
    }
}
} // namespace

TEST("async section - an async invocation inside a section is addressed below that section", no_scheduler)
{
    for (auto const& filters : {cc::vector<cc::string>{}, cc::vector<cc::string>{"dispatch", "g", "child"}})
    {
        probe p;
        nx::test_registry reg;
        reg.add_async_invocable_declaration("child", {}, cc::arg_types_of(cc::signature_of<decltype(&invocable_child)>{}),
                                            nx::impl::make_async_test_invoker(&invocable_child));
        reg.add_async_declaration("driver", {}, [&p](nx::impl::async_test_sink& sink)
                                  { nx::impl::submit_test_async(sink, invokes_inside_a_section(&p)); });

        auto config = nx::test_schedule_config{};
        config.jobs = 4;
        config.section_filters = filters;
        auto const schedule = nx::test_schedule::create({}, reg);
        auto const exec = nx::execute_tests(schedule, config);

        CHECK(exec.count_failed_tests() == 0);
        CHECK(p.invoked.matched == 1);
        CHECK(p.invoked.executed == 1);
    }
}
