#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <string>
#include <vector>

// Meta-tests: build a local registry with invocable tests + a driver, run it via execute_tests, and
// inspect the resulting (nested) execution tree. Using a local registry keeps these tests out of the
// static registry, so nx::invoke_tests (which targets the active run's registry) only sees what each test adds.

namespace
{
// Adds an invocable (inert) test to a local registry from a (possibly capturing) callable. Mirrors what
// the INVOCABLE_TEST macro does for the static registry, but accepts lambdas so tests can observe side effects.
template <class Fn>
void add_invocable(nx::test_registry& reg, cc::string name, Fn fn, nx::config::cfg cfg = {})
{
    using sig = cc::signature_of<Fn>;
    reg.add_invocable_declaration(cc::move(name), cfg, cc::arg_types_of(sig{}),
                                  [fn = cc::move(fn)](cc::span<nx::typed_value*> in)
                                  { nx::impl::invoke_with_values(fn, in, sig{}); });
}
} // namespace

TEST("invocable tests - invoke_tests drives matching instances once, nested under the driver")
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

TEST("invocable tests - one dataset drives several different invocable tests")
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

TEST("invocable tests - matching is on decayed types (T and T const& are the same key)")
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

TEST("invocable tests - nesting: an invoked test can itself invoke tests")
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

TEST("invocable tests - section filters scope invocation to a single instance")
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

TEST("invocable tests - a failing instance fails the run and is addressable, siblings still pass")
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

TEST("invocable tests - an uninvoked invocable test leaves no nested execution (orphan signal)")
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
