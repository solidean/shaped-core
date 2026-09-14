#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

// ASYNC_INVOCABLE_TEST and the two async invocations.
//
// Meta-tests, like test-invocable-test.cc: each builds a local registry, runs it nested, and reads the execution tree.
// Every child is a coroutine taking the probe by const&, which is also what pins the box lifetime: a child that
// suspends reads its argument after the invocation's own call returned.

using namespace cc::primitive_defines;

namespace
{
struct probe
{
    cc::mutex<cc::vector<cc::string>> order;
    cc::atomic<int> inside = {0};
    cc::atomic<int> most_inside = {0};
    cc::atomic<u64> thread = {0};
    cc::shared_async<int> latch = cc::make_async_manual<int>();
    nx::invocation_result result;
};

// The join key: a struct holding the probe, since a typed_value refuses a raw pointer on its own.
struct probe_key
{
    probe* p = nullptr;
};

void note(probe_key const& k, cc::string_view name)
{
    k.p->order.lock([&](cc::vector<cc::string>& o) { o.push_back(cc::string(name)); });
}

/// A dependency that finishes on the compute pool a little later, so whoever awaits it genuinely suspends.
cc::shared_async<int> a_moment_later()
{
    return cc::make_async_scheduled(
        []
        {
            cc::this_thread_sleep_secs(0.003);
            return 1;
        });
}

cc::shared_async<cc::unit> child_a(probe_key const& k)
{
    note(k, "a");
    CHECK(true);
    co_return;
}

cc::shared_async<cc::unit> child_c(probe_key const& k)
{
    (void)co_await a_moment_later();
    note(k, "c"); // read through the box after a suspend
    CHECK(true);
}

/// Counts how many children are inside at once.
cc::shared_async<cc::unit> child_counting(probe_key const& k)
{
    auto const now = k.p->inside.fetch_add(1) + 1;
    auto seen = k.p->most_inside.load();
    while (now > seen && !k.p->most_inside.compare_exchange_weak(seen, now))
    {
    }
    (void)co_await a_moment_later();
    k.p->inside.fetch_sub(1);
    CHECK(true);
}

cc::shared_async<cc::unit> child_waits(probe_key const& k)
{
    CHECK(co_await k.p->latch == 7); // resolved only by a sibling that is running at the same time
    note(k, "waited");
}

cc::shared_async<cc::unit> child_releases(probe_key const& k)
{
    (void)co_await a_moment_later();
    note(k, "released"); // before the push: the waiter can resume on another thread the moment it lands
    k.p->latch->push_value(7);
    CHECK(true);
}

cc::shared_async<cc::unit> child_records_thread(probe_key const& k)
{
    k.p->thread.store(u64(cc::current_thread_id()));
    CHECK(true);
    co_return;
}

cc::shared_async<cc::unit> drive_in_sequence(probe_key k)
{
    k.p->result = co_await nx::async_invoke_tests_in_sequence("g", k);
}

cc::shared_async<cc::unit> drive_in_parallel(probe_key k)
{
    k.p->result = co_await nx::async_invoke_tests_in_parallel("g", k);
}

template <class F>
void add_async_child(nx::test_registry& reg, cc::string name, F* fn, nx::config::cfg cfg = {})
{
    reg.add_async_invocable_declaration(cc::move(name), cfg, cc::arg_types_of(cc::signature_of<F*>{}),
                                        nx::impl::make_async_test_invoker(fn));
}

template <class Fn>
void add_sync_child(nx::test_registry& reg, cc::string name, Fn fn, nx::config::cfg cfg = {})
{
    using sig = cc::signature_of<Fn>;
    reg.add_invocable_declaration(cc::move(name), cfg, cc::arg_types_of(sig{}),
                                  [fn = cc::move(fn)](cc::span<nx::typed_value*> in)
                                  { nx::impl::invoke_with_values(fn, in, sig{}); });
}

nx::test_schedule_execution run_driver(nx::test_registry& reg,
                                       probe& p,
                                       cc::shared_async<cc::unit> (*driver)(probe_key),
                                       int jobs,
                                       nx::config::cfg driver_cfg = {},
                                       cc::vector<cc::string> section_filters = {})
{
    reg.add_async_declaration("driver", driver_cfg, [&p, driver](nx::impl::async_test_sink& sink)
                              { nx::impl::submit_test_async(sink, driver(probe_key{&p})); });

    auto config = nx::test_schedule_config{};
    config.jobs = jobs;
    config.section_filters = cc::move(section_filters);
    auto const schedule = nx::test_schedule::create({}, reg);
    return nx::execute_tests(schedule, config);
}

bool any_error_mentions(nx::test_execution const& e, cc::string_view text)
{
    auto found = false;
    for (auto const& err : e.root.errors)
        found |= err.expr.contains(text) || err.expanded.contains(text);
    return found;
}
} // namespace

TEST("async invocables - in_sequence runs sync and async children in match order, nested under the driver", no_scheduler)
{
    for (auto const jobs : {1, 4})
    {
        probe p;
        nx::test_registry reg;
        add_async_child(reg, "a", &child_a);
        add_sync_child(reg, "b",
                       [](probe_key const& k)
                       {
                           note(k, "b");
                           CHECK(true);
                       });
        add_async_child(reg, "c", &child_c);

        auto const exec = run_driver(reg, p, &drive_in_sequence, jobs);

        CHECK(exec.count_failed_tests() == 0);
        CHECK(p.result.matched == 3);
        CHECK(p.result.executed == 3);

        auto const order = p.order.lock([](cc::vector<cc::string>& o) { return o; });
        REQUIRE(order.size() == 3);
        CHECK(order[0] == "a");
        CHECK(order[1] == "b");
        CHECK(order[2] == "c");

        REQUIRE(exec.executions.size() == 1);
        auto const& driver = exec.executions[0];
        REQUIRE(driver.nested.size() == 3);
        CHECK(driver.nested[0].instance.declaration->name == "a");
        CHECK(driver.nested[2].instance.declaration->name == "c");
        CHECK(driver.nested[2].invocation_group == "g");
        CHECK(!driver.root.is_considered_failing); // its checks live in its children
    }
}

TEST("async invocables - in_parallel starts every child before awaiting any, and reports in match order", no_scheduler)
{
    // Under a serial drive the first child would wait forever for a latch only the second child pushes.
    probe p;
    nx::test_registry reg;
    add_async_child(reg, "a - waits", &child_waits);
    add_async_child(reg, "b - releases", &child_releases);

    auto const exec = run_driver(reg, p, &drive_in_parallel, 4);

    CHECK(exec.count_failed_tests() == 0);
    CHECK(p.result.executed == 2);
    REQUIRE(exec.executions.size() == 1);
    REQUIRE(exec.executions[0].nested.size() == 2);
    CHECK(exec.executions[0].nested[0].instance.declaration->name == "a - waits");

    auto const order = p.order.lock([](cc::vector<cc::string>& o) { return o; });
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "released"); // finished out of match order, reported in it
}

TEST("async invocables - in_parallel runs its children one at a time under -j1", no_scheduler)
{
    probe p;
    nx::test_registry reg;
    add_async_child(reg, "one", &child_counting);
    add_async_child(reg, "two", &child_counting);
    add_async_child(reg, "three", &child_counting);

    auto const exec = run_driver(reg, p, &drive_in_parallel, 1);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(p.result.executed == 3);
    CHECK(p.most_inside.load() == 1);
}

TEST("async invocables - parallel siblings sharing a tag take turns through the phase lock", no_scheduler)
{
    probe p;
    nx::test_registry reg;
    auto const tagged = nx::impl::merge_config(nx::config::exclusive("async-invocables-shared"));
    add_async_child(reg, "one", &child_counting, tagged);
    add_async_child(reg, "two", &child_counting, tagged);
    add_async_child(reg, "three", &child_counting, tagged);

    auto const exec = run_driver(reg, p, &drive_in_parallel, 4);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(p.result.executed == 3);
    CHECK(p.most_inside.load() == 1);
}

TEST("async invocables - a child asking for main_thread runs on main under a driver that does not", no_scheduler)
{
    REQUIRE(cc::current_thread_id() == cc::thread_id::main);
    auto const on_main = nx::impl::merge_config(nx::config::main_thread);

    {
        probe p;
        nx::test_registry reg;
        add_async_child(reg, "async on main", &child_records_thread, on_main);
        auto const exec = run_driver(reg, p, &drive_in_sequence, 4);
        CHECK(exec.count_failed_tests() == 0);
        CHECK(p.thread.load() == u64(cc::thread_id::main));
    }
    {
        probe p;
        nx::test_registry reg;
        add_sync_child(
            reg, "sync on main",
            [](probe_key const& k)
            {
                k.p->thread.store(u64(cc::current_thread_id()));
                CHECK(true);
            },
            on_main);
        auto const exec = run_driver(reg, p, &drive_in_parallel, 4);
        CHECK(exec.count_failed_tests() == 0);
        CHECK(p.thread.load() == u64(cc::thread_id::main));
    }
}

TEST("async invocables - -c addresses one async child under its invocation group", no_scheduler)
{
    probe p;
    nx::test_registry reg;
    add_async_child(reg, "a", &child_a);
    add_async_child(reg, "c", &child_c);

    auto const exec = run_driver(reg, p, &drive_in_sequence, 1, {}, {"g", "c"});
    CHECK(exec.count_failed_tests() == 0);
    CHECK(p.result.matched == 2);
    CHECK(p.result.executed == 1);

    auto const order = p.order.lock([](cc::vector<cc::string>& o) { return o; });
    REQUIRE(order.size() == 1);
    CHECK(order[0] == "c");
}

TEST("async invocables - an async invocation's asks: never both tags, and no exclusive() among parallel siblings")
{
    using nx::config::exclusive;
    using nx::config::main_thread;
    using nx::config::singlethreaded;
    auto const make = [](auto&&... items) { return nx::impl::merge_config(items...); };
    auto const why = [](nx::config::cfg const& child, nx::config::cfg const& slot, bool held, bool parallel)
    { return nx::impl::find_unhonoured_async_dispatch_config(child, slot, held, parallel); };

    // Arranged by the invocation rather than inherited.
    CHECK(why(make(main_thread), make(), false, false).empty());
    CHECK(why(make(exclusive("gpu")), make(), false, true).empty());

    // A tag on each side of the chain is the out-of-order acquisition that deadlocks.
    CHECK(why(make(exclusive("gpu")), make(exclusive("net")), true, false).contains("never both"));

    // exclusive() still needs an exclusive() driver, and alone among siblings is not something a fan-out can give.
    CHECK(!why(make(exclusive()), make(), false, false).empty());
    CHECK(why(make(exclusive()), make(exclusive()), false, false).empty());
    CHECK(!why(make(exclusive()), make(exclusive()), false, true).empty());

    // Scheduler modes stay the driver's to share.
    CHECK(why(make(singlethreaded), make(), false, false).contains("singlethreaded"));
    CHECK(why(make(singlethreaded), make(singlethreaded), false, false).empty());
}

#if CC_ASSERT_ENABLED
TEST("async invocables - a tagged child under a tag-holding driver fails the driver without running", no_scheduler)
{
    probe p;
    nx::test_registry reg;
    add_async_child(reg, "tagged", &child_a, nx::impl::merge_config(nx::config::exclusive("child-tag")));

    auto const exec
        = run_driver(reg, p, &drive_in_sequence, 1, nx::impl::merge_config(nx::config::exclusive("driver-tag")));
    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.executions[0].is_considered_failing());
    CHECK(any_error_mentions(exec.executions[0], "never both"));
    CHECK(p.order.lock([](cc::vector<cc::string>& o) { return o.size(); }) == 0);
}

TEST("async invocables - a synchronous invoke_tests refuses a matched async invocable", no_scheduler)
{
    auto sync_ran = 0;
    probe p;
    nx::test_registry reg;
    add_async_child(reg, "async", &child_a);
    add_sync_child(reg, "sync",
                   [&](probe_key const&)
                   {
                       ++sync_ran;
                       CHECK(true);
                   });
    reg.add_declaration("sync driver", {}, [&] { nx::invoke_tests("g", probe_key{&p}); });

    auto const schedule = nx::test_schedule::create({}, reg);
    auto const exec = nx::execute_tests(schedule, {});
    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.executions[0].is_considered_failing());
    CHECK(sync_ran == 0); // refused on the matched set, before any child runs
}
#endif

// The macro end to end, in the static registry: the driver below is what keeps the child from being an orphan.
namespace
{
struct macro_key
{
    int value = 0;
};
} // namespace

ASYNC_INVOCABLE_TEST("async invocables - a macro-declared coroutine child reads its argument after a suspend",
                     (macro_key const& k))
{
    (void)co_await a_moment_later();
    CHECK(k.value == 42);
}

ASYNC_TEST("async invocables - an ASYNC_TEST drives the macro-declared child")
{
    auto const r = co_await nx::async_invoke_tests_in_sequence("macro", macro_key{42});
    CHECK(r.executed == 1);
}
