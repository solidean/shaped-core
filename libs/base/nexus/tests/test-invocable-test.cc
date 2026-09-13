#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <string>
#include <vector>

// Meta-tests: build a local registry with invocable tests plus a driver, run it via execute_tests, and inspect the resulting (nested) execution tree.
// A local registry keeps these tests out of the static registry, so nx::invoke_tests — which targets the active run's registry — only sees what each test adds.

namespace
{
// Adds an invocable (inert) test to a local registry from a possibly-capturing callable.
// Mirrors what the INVOCABLE_TEST macro does for the static registry, but accepts lambdas so a test can observe side effects.
template <class Fn>
void add_invocable(nx::test_registry& reg, cc::string name, Fn fn, nx::config::cfg cfg = {})
{
    using sig = cc::signature_of<Fn>;
    reg.add_invocable_declaration(cc::move(name), cfg, cc::arg_types_of(sig{}),
                                  [fn = cc::move(fn)](cc::span<nx::typed_value*> in)
                                  { nx::impl::invoke_with_values(fn, in, sig{}); });
}
} // namespace

TEST("invocable tests - invoke_tests drives matching instances once, nested under the driver", no_scheduler)
{
    std::vector<int> seen;

    nx::test_registry reg;
    add_invocable(reg, "square nonneg",
                  [&](int x)
                  {
                      seen.push_back(x);
                      CHECK(x * x >= 0);
                  });
    reg.add_declaration("driver", {},
                        [&]
                        {
                            nx::invoke_tests("vals", 2); // template arg deduced (the default) -> int
                            nx::invoke_tests("vals", 3);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // the invocable test body ran once per call, in order
    auto const expected_seen = std::vector<int>{2, 3};
    CHECK(seen == expected_seen);

    // driver + 2 invoked instances
    CHECK(exec.count_total_tests() == 3);
    CHECK(exec.count_failed_tests() == 0);
    CHECK(exec.count_failed_checks() == 0);

    // only the driver is a top-level instance; the two instances are nested under it
    REQUIRE(exec.executions.size() == 1);
    auto const& driver = exec.executions[0];
    REQUIRE(driver.nested.size() == 2);
    CHECK(driver.nested[0].invocation_group == "vals");
    CHECK(driver.nested[0].instance.declaration->name == "square nonneg");
    // a driver with no CHECK of its own is not flagged when it invoked children
    CHECK(!driver.root.is_considered_failing);
}

TEST("invocable tests - one dataset drives several different invocable tests", no_scheduler)
{
    struct mesh_case
    {
        int verts = 0;
    };

    int ran_a = 0;
    int ran_b = 0;

    nx::test_registry reg;
    add_invocable(reg, "aspect A",
                  [&](mesh_case const& c)
                  {
                      ++ran_a;
                      CHECK(c.verts == 7);
                  });
    add_invocable(reg, "aspect B",
                  [&](mesh_case const& c)
                  {
                      ++ran_b;
                      CHECK(c.verts > 0);
                  });
    reg.add_declaration("cases", {}, [&] { nx::invoke_tests<mesh_case const&>("case-7", mesh_case{7}); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(ran_a == 1);
    CHECK(ran_b == 1);
    CHECK(exec.count_total_tests() == 3); // driver + 2 aspects
    CHECK(exec.count_failed_tests() == 0);

    REQUIRE(exec.executions.size() == 1);
    // matches are ordered by name: "aspect A" before "aspect B"
    REQUIRE(exec.executions[0].nested.size() == 2);
    CHECK(exec.executions[0].nested[0].instance.declaration->name == "aspect A");
    CHECK(exec.executions[0].nested[1].instance.declaration->name == "aspect B");
}

TEST("invocable tests - matching is on decayed types (T and T const& are the same key)", no_scheduler)
{
    struct thing
    {
        int v = 0;
    };

    int by_ref = 0;

    nx::test_registry reg;
    add_invocable(reg, "takes const-ref",
                  [&](thing const& t)
                  {
                      ++by_ref;
                      CHECK(t.v == 5);
                  });
    reg.add_declaration("drive-by-value", {}, [&] { nx::invoke_tests<thing>("g", thing{5}); });
    reg.add_declaration("drive-by-ref", {}, [&] { nx::invoke_tests<thing const&>("g", thing{5}); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // both drivers reach the same (decayed) invocable test
    CHECK(by_ref == 2);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("invocable tests - nesting: an invoked test can itself invoke tests", no_scheduler)
{
    std::vector<std::string> log;

    nx::test_registry reg;
    add_invocable(reg, "leaf",
                  [&](double d)
                  {
                      log.emplace_back("leaf");
                      CHECK(d > 0);
                  });
    add_invocable(reg, "mid",
                  [&](int x)
                  {
                      log.emplace_back("mid");
                      CHECK(x == 1);
                      nx::invoke_tests<double>("inner", 2.0);
                  });
    reg.add_declaration("top", {}, [&] { nx::invoke_tests<int>("outer", 1); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    auto const expected_log = std::vector<std::string>{"mid", "leaf"};
    CHECK(log == expected_log);
    CHECK(exec.count_total_tests() == 3); // top + mid + leaf
    CHECK(exec.count_failed_tests() == 0);

    REQUIRE(exec.executions.size() == 1);
    REQUIRE(exec.executions[0].nested.size() == 1); // mid
    CHECK(exec.executions[0].nested[0].invocation_group == "outer");
    REQUIRE(exec.executions[0].nested[0].nested.size() == 1); // leaf under mid
    CHECK(exec.executions[0].nested[0].nested[0].invocation_group == "inner");
}

TEST("invocable tests - section filters scope invocation to a single instance", no_scheduler)
{
    int ran_a = 0;
    int ran_b = 0;

    nx::test_registry reg;
    add_invocable(reg, "childA",
                  [&](int)
                  {
                      ++ran_a;
                      CHECK(true);
                  });
    add_invocable(reg, "childB",
                  [&](int)
                  {
                      ++ran_b;
                      CHECK(true);
                  });
    reg.add_declaration("driver", {}, [&] { nx::invoke_tests<int>("group", 0); });

    // address a single instance: driver / group / childB
    nx::test_schedule_config config;
    config.section_filters = {"group", "childB"};

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, config);

    CHECK(ran_a == 0);
    CHECK(ran_b == 1);

    REQUIRE(exec.executions.size() == 1);
    REQUIRE(exec.executions[0].nested.size() == 1);
    CHECK(exec.executions[0].nested[0].instance.declaration->name == "childB");
}

TEST("invocable tests - a failing instance fails the run and is addressable, siblings still pass", no_scheduler)
{
    nx::test_registry reg;
    add_invocable(reg, "ok", [&](int x) { CHECK(x == x); });
    add_invocable(reg, "bad", [&](int) { CHECK(1 == 2); });
    reg.add_declaration("driver", {}, [&] { nx::invoke_tests<int>("group", 1); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // exactly the "bad" instance fails
    CHECK(exec.count_total_tests() == 3);
    CHECK(exec.count_failed_tests() == 1);
    CHECK(exec.count_failed_checks() == 1);
    CHECK(exec.executions[0].is_considered_failing()); // driver considered failing via its child

    REQUIRE(exec.executions[0].nested.size() == 2);
    // ordered by name: "bad" before "ok"
    CHECK(exec.executions[0].nested[0].instance.declaration->name == "bad");
    CHECK(exec.executions[0].nested[0].root.is_considered_failing);
    CHECK(!exec.executions[0].nested[1].root.is_considered_failing);
}

TEST("invocable tests - an uninvoked invocable test leaves no nested execution (orphan signal)", no_scheduler)
{
    nx::test_registry reg;
    add_invocable(reg, "never run", [&](int) { CHECK(true); });
    reg.add_declaration("driver", {}, [&] { CHECK(true); }); // invokes nothing

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // the invocable test is inert and was not scheduled; the driver ran but invoked nothing, so no
    // execution references the invocable declaration — this is exactly what the orphan check detects.
    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.executions[0].nested.empty());
    CHECK(exec.count_total_tests() == 1);
}

TEST("invocable tests - an invocable's SECTIONs are explored inside invoke_tests; the driver runs once", no_scheduler)
{
    int driver_runs = 0;
    int contexts_built = 0;
    int invocable_runs = 0;
    std::vector<std::string> visited;

    nx::test_registry reg;
    add_invocable(reg, "sectioned",
                  [&](int ctx_id)
                  {
                      ++invocable_runs;
                      CHECK(ctx_id == 1); // the same context is handed to every section pass

                      SECTION("sec A")
                      {
                          visited.emplace_back("A");
                          SUCCEED();
                      }
                      SECTION("sec B")
                      {
                          visited.emplace_back("B");
                          SUCCEED();
                      }
                  });
    reg.add_declaration("driver", {},
                        [&]
                        {
                            ++driver_runs;
                            int const ctx_id = ++contexts_built; // "build the context" — must happen exactly once
                            nx::invoke_tests("run", ctx_id);
                        });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    // section-replay of the invocable happens inside invoke_tests, not by re-running the driver: the driver
    // body (and thus context construction) runs exactly once, while the invocable body re-runs per section.
    CHECK(driver_runs == 1);
    CHECK(contexts_built == 1);
    CHECK(invocable_runs == 2);
    auto const expected_visited = std::vector<std::string>{"A", "B"};
    CHECK(visited == expected_visited);
    CHECK(exec.count_failed_tests() == 0);
}

TEST("invocable tests - a self-invoking invocable is caught by the cycle guard (no infinite recursion)", no_scheduler)
{
    struct cyc
    {
        int v = 0;
    };
    int runs = 0;

    nx::test_registry reg;
    add_invocable(reg, "self",
                  [&](cyc c)
                  {
                      ++runs;
                      CHECK(true);
                      nx::invoke_tests<cyc>("again", cyc{c.v + 1}); // matches "self" again -> would recurse forever
                  });
    reg.add_declaration("driver", {}, [&] { nx::invoke_tests<cyc>("start", cyc{0}); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(runs == 1);                      // ran once; the recursive invoke was refused instead of looping
    CHECK(exec.count_failed_tests() >= 1); // the cycle surfaces as a failure, not a silent no-op
}

TEST("invocable tests - an indirect invocation cycle (A -> B -> A) is caught", no_scheduler)
{
    struct ta
    {
    };
    struct tb
    {
    };
    int a_runs = 0;
    int b_runs = 0;

    nx::test_registry reg;
    add_invocable(reg, "A",
                  [&](ta)
                  {
                      ++a_runs;
                      CHECK(true);
                      nx::invoke_tests<tb>("to-b", tb{});
                  });
    add_invocable(reg, "B",
                  [&](tb)
                  {
                      ++b_runs;
                      CHECK(true);
                      nx::invoke_tests<ta>("to-a", ta{}); // A is already running above -> cycle
                  });
    reg.add_declaration("driver", {}, [&] { nx::invoke_tests<ta>("start", ta{}); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(a_runs == 1); // each runs once; the back-edge into A is refused
    CHECK(b_runs == 1);
    CHECK(exec.count_failed_tests() >= 1);
}

TEST("invocable tests - a dispatched child's scheduling asks are honoured only by the slot it runs in")
{
    using nx::config::exclusive;
    using nx::config::main_thread;
    using nx::config::own_pool;
    using nx::config::singlethreaded;
    auto const make = [](auto&&... items) { return nx::impl::merge_config(items...); };
    auto const unhonoured = [](nx::config::cfg const& child, nx::config::cfg const& slot)
    { return nx::impl::find_unhonoured_dispatch_config(child, slot); };

    // A child asking for nothing runs anywhere, whatever the slot holds.
    CHECK(unhonoured(make(), make()).empty());
    CHECK(unhonoured(make(), make(exclusive("gpu"), singlethreaded)).empty());

    // Exclusion: every tag must be held by the slot, or the slot must run alone.
    CHECK(unhonoured(make(exclusive("gpu")), make()) == "exclusive(\"gpu\")");
    CHECK(unhonoured(make(exclusive("gpu")), make(exclusive("gpu"))).empty());
    CHECK(unhonoured(make(exclusive("gpu")), make(exclusive("net"), exclusive("gpu"))).empty());
    CHECK(unhonoured(make(exclusive("gpu"), exclusive("net")), make(exclusive("gpu"))) == "exclusive(\"net\")");
    CHECK(unhonoured(make(exclusive("gpu")), make(exclusive())).empty());
    CHECK(unhonoured(make(exclusive()), make(exclusive("gpu"))) == "exclusive()");

    // main_thread is a flag the slot has to carry too.
    CHECK(unhonoured(make(main_thread), make()) == "main_thread");
    CHECK(unhonoured(make(main_thread), make(main_thread)).empty());

    // A scheduler mode other than the default must be the slot's exactly.
    CHECK(unhonoured(make(singlethreaded), make()) == "singlethreaded");
    CHECK(unhonoured(make(singlethreaded), make(singlethreaded)).empty());
    CHECK(unhonoured(make(nx::config::no_scheduler), make(singlethreaded)) == "no_scheduler");
    CHECK(unhonoured(make(own_pool(2)), make(own_pool(4))) == "own_pool(2)");
    CHECK(unhonoured(make(own_pool(2)), make(own_pool(2))).empty());
}

#if CC_ASSERT_ENABLED
TEST("invocable tests - dispatching a child whose exclusion the driver does not hold fails the driver", no_scheduler)
{
    struct tagged_key
    {
    };
    auto child_runs = 0;

    nx::test_registry reg;
    add_invocable(
        reg, "tagged child",
        [&](tagged_key)
        {
            ++child_runs;
            CHECK(true);
        },
        nx::impl::merge_config(nx::config::exclusive("shared-state")));
    reg.add_declaration("holding driver", nx::impl::merge_config(nx::config::exclusive("shared-state")),
                        [&] { nx::invoke_tests("run", tagged_key{}); });
    reg.add_declaration("bare driver", {}, [&] { nx::invoke_tests("run", tagged_key{}); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(child_runs == 1); // only under the driver holding the tag

    nx::test_execution const* holding = nullptr;
    nx::test_execution const* bare = nullptr;
    for (auto const& e : exec.executions)
    {
        if (e.instance.declaration->name == "bare driver")
            bare = &e;
        else
            holding = &e;
    }
    REQUIRE(holding != nullptr);
    REQUIRE(bare != nullptr);

    CHECK(!holding->is_considered_failing());
    CHECK(holding->nested.size() == 1);

    // The assert fires before the child runs, so the driver fails with no child under it.
    CHECK(bare->is_considered_failing());
    CHECK(bare->nested.empty());
}

TEST("invocable tests - a nested dispatch is checked against the scheduled test, not the child between", no_scheduler)
{
    struct outer_key
    {
    };
    struct inner_key
    {
    };
    auto leaf_runs = 0;

    nx::test_registry reg;
    add_invocable(
        reg, "leaf",
        [&](inner_key)
        {
            ++leaf_runs;
            CHECK(true);
        },
        nx::impl::merge_config(nx::config::singlethreaded));
    // The middle child asks for nothing, and still dispatches the leaf: what runs the leaf is the driver's slot.
    add_invocable(reg, "middle", [&](outer_key) { nx::invoke_tests("inner", inner_key{}); });
    reg.add_declaration("driver", nx::impl::merge_config(nx::config::singlethreaded),
                        [&] { nx::invoke_tests("outer", outer_key{}); });

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});

    CHECK(leaf_runs == 1);
    CHECK(exec.count_failed_tests() == 0);
}
#endif

// --- static end-to-end smoke of the INVOCABLE_TEST macro + real nx::invoke_tests against the static registry.
// The driver must invoke this, or a full unfiltered run would report it as an orphan.

INVOCABLE_TEST("invocable macro smoke - value doubles", (int x))
{
    CHECK(x + x == 2 * x);
}

TEST("invocable macro smoke - driver")
{
    nx::invoke_tests("smoke", 21); // deduced -> int
}
