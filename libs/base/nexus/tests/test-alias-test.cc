#include <clean-core/common/utility.hh>
#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <string>
#include <vector>

// Meta-tests for aliases: build a local registry with invocable tests + per-"backend" driver tests, define
// aliases against it (via the nx::setup API), then schedule/execute under a filter and inspect the tree.
// Using a local registry keeps these out of the static registry (aliases + invoke_tests target the active run's
// registry). Mirrors test-invocable-test.cc; see that file for the invocable machinery this builds on.

namespace
{
// Adds an invocable (inert) test to a local registry from a (possibly capturing) callable. Same helper as in
// test-invocable-test.cc.
template <class Fn>
void add_invocable(nx::test_registry& reg, cc::string name, Fn fn, nx::config::cfg cfg = {})
{
    using sig = cc::signature_of<Fn>;
    reg.add_invocable_declaration(cc::move(name), cfg, cc::arg_types_of(sig{}),
                                  [fn = cc::move(fn)](cc::span<nx::typed_value*> in)
                                  { nx::impl::invoke_with_values(fn, in, sig{}); });
}

// Config selecting tests/aliases by the given name filter (as the CLI does for a bare pattern).
nx::test_schedule_config with_filter(cc::string filter)
{
    nx::test_schedule_config config;
    config.filters.push_back(cc::move(filter));
    // No bucket flag was given, so an exactly-named test may cross buckets (mirrors create_from_args).
    // run_disabled_tests is the bulk "disabled too" request — set here so these alias tests exercise disabled
    // drivers under a substring filter too, which alone would not enable them.
    config.allow_cross_bucket_naming = true;
    config.run_disabled_tests = true;
    return config;
}
} // namespace

TEST("aliases - setup API selects invocables by decayed signature and records aliases")
{
    nx::test_registry reg;
    add_invocable(reg, "ctx test A", [&](int) { CHECK(true); });
    add_invocable(reg, "ctx test B", [&](int const&) { CHECK(true); }); // decays to the same key as int
    add_invocable(reg, "other", [&](double) { CHECK(true); });
    reg.add_declaration("driver", {}, [&] {});

    nx::setup s(reg);

    // all declarations are visible
    CHECK(s.tests().size() == 4);
    CHECK(s.find_test("driver") != nullptr);
    CHECK(s.find_test("nope") == nullptr);

    // matching is on the decayed signature: int and int const& are the same key, double is not
    auto const int_invocables = s.invocables_with<int>();
    CHECK(int_invocables.size() == 2);
    CHECK(s.invocables_with<double>().size() == 1);

    s.define_alias("ctx test A",
                   {nx::alias_fragment{.driver = s.find_test("driver"), .section_path = {"g", "ctx test A"}}});
    REQUIRE(reg.aliases.size() == 1);
    CHECK(reg.aliases[0].name == "ctx test A");
    REQUIRE(reg.aliases[0].fragments.size() == 1);
    CHECK(reg.aliases[0].fragments[0].driver == s.find_test("driver"));
}

TEST("aliases - a filter matching an alias expands to one scoped instance")
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
    reg.add_declaration("drv", {}, [&] { nx::invoke_tests<int>("grp", 0); });

    nx::setup s(reg);
    s.define_alias("childB", {nx::alias_fragment{.driver = s.find_test("drv"), .section_path = {"grp", "childB"}}});

    // filter matches the alias name (not the driver name "drv"), so the only scheduled instance is the alias's
    auto schedule = nx::test_schedule::create(with_filter("childB"), reg);
    REQUIRE(schedule.instances.size() == 1);
    CHECK(schedule.instances[0].declaration == s.find_test("drv"));
    REQUIRE(schedule.instances[0].section_scopes.size() == 1);
    REQUIRE(schedule.instances[0].section_scopes[0].size() == 2);
    CHECK(schedule.instances[0].section_scopes[0][0] == "grp");
    CHECK(schedule.instances[0].section_scopes[0][1] == "childB");

    auto exec = nx::execute_tests(schedule, with_filter("childB"));

    // only childB ran, via the driver, scoped by the fragment path
    CHECK(ran_a == 0);
    CHECK(ran_b == 1);
    REQUIRE(exec.executions.size() == 1);
    REQUIRE(exec.executions[0].nested.size() == 1);
    CHECK(exec.executions[0].nested[0].instance.declaration->name == "childB");
}

TEST("aliases - a multi-fragment alias runs each fragment scoped to its own driver")
{
    std::vector<std::string> ran_under;

    nx::test_registry reg;
    add_invocable(reg, "shared",
                  [&](int)
                  {
                      ran_under.emplace_back("x");
                      CHECK(true);
                  });
    reg.add_declaration("drvA", {}, [&] { nx::invoke_tests<int>("a", 0); });
    reg.add_declaration("drvB", {}, [&] { nx::invoke_tests<int>("b", 0); });

    nx::setup s(reg);
    s.define_alias("shared", {
                                 nx::alias_fragment{.driver = s.find_test("drvA"), .section_path = {"a", "shared"}},
                                 nx::alias_fragment{.driver = s.find_test("drvB"), .section_path = {"b", "shared"}},
                             });

    auto schedule = nx::test_schedule::create(with_filter("shared"), reg);
    REQUIRE(schedule.instances.size() == 2); // one per backend driver

    auto exec = nx::execute_tests(schedule, with_filter("shared"));

    // the shared invocable ran once under each driver
    CHECK(ran_under.size() == 2);
    CHECK(exec.count_total_tests() == 4); // 2 drivers + 2 shared instances
    CHECK(exec.count_failed_tests() == 0);
    REQUIRE(exec.executions.size() == 2);
    REQUIRE(exec.executions[0].nested.size() == 1);
    REQUIRE(exec.executions[1].nested.size() == 1);
    CHECK(exec.executions[0].nested[0].instance.declaration->name == "shared");
    CHECK(exec.executions[1].nested[0].instance.declaration->name == "shared");
}

TEST("aliases - many aliases of one driver collapse into a single scoped run")
{
    // The "sg -" case: one backend driver, several invocables, an alias per invocable. A filter matching the
    // alias names (but NOT the driver name) must schedule the driver ONCE — with every matched path in its
    // scope set — never once per alias. Aliases are pure filters, not additive schedule entries.
    int drv_runs = 0;
    int ran_a = 0;
    int ran_b = 0;
    int ran_c = 0;
    int ran_d = 0;

    nx::test_registry reg;
    add_invocable(reg, "sel A",
                  [&](int)
                  {
                      ++ran_a;
                      CHECK(true);
                  });
    add_invocable(reg, "sel B",
                  [&](int)
                  {
                      ++ran_b;
                      CHECK(true);
                  });
    add_invocable(reg, "sel C",
                  [&](int)
                  {
                      ++ran_c;
                      CHECK(true);
                  });
    add_invocable(reg, "sel D",
                  [&](int)
                  {
                      ++ran_d;
                      CHECK(true);
                  }); // not aliased → must stay filtered out
    reg.add_declaration("backend", {},
                        [&]
                        {
                            ++drv_runs;
                            nx::invoke_tests<int>("g", 0);
                        });

    nx::setup s(reg);
    s.define_alias("pick sel A", {nx::alias_fragment{.driver = s.find_test("backend"), .section_path = {"g", "sel A"}}});
    s.define_alias("pick sel B", {nx::alias_fragment{.driver = s.find_test("backend"), .section_path = {"g", "sel B"}}});
    s.define_alias("pick sel C", {nx::alias_fragment{.driver = s.find_test("backend"), .section_path = {"g", "sel C"}}});

    // "pick sel" matches all three alias names, none of which is the driver name "backend".
    auto schedule = nx::test_schedule::create(with_filter("pick sel"), reg);
    REQUIRE(schedule.instances.size() == 1); // ONE driver instance, not three
    CHECK(schedule.instances[0].declaration == s.find_test("backend"));
    CHECK(schedule.instances[0].section_scopes.size() == 3); // all three selected paths, OR-matched at run time

    auto exec = nx::execute_tests(schedule, with_filter("pick sel"));
    CHECK(drv_runs == 1); // the driver body (and its one-time setup) ran exactly once
    CHECK(ran_a == 1);
    CHECK(ran_b == 1);
    CHECK(ran_c == 1);
    CHECK(ran_d == 0); // the unaliased invocable stayed filtered out
    REQUIRE(exec.executions.size() == 1);
    CHECK(exec.executions[0].nested.size() == 3); // A, B, C dispatched under the single driver run
}

TEST("aliases - duplicate fragments (same driver + path) are scheduled once")
{
    nx::test_registry reg;
    add_invocable(reg, "child", [&](int) { CHECK(true); });
    reg.add_declaration("drv", {}, [&] { nx::invoke_tests<int>("grp", 0); });

    nx::setup s(reg);
    // two identical fragments — the scheduler must dedupe on (driver, section path)
    s.define_alias("child", {
                                nx::alias_fragment{.driver = s.find_test("drv"), .section_path = {"grp", "child"}},
                                nx::alias_fragment{.driver = s.find_test("drv"), .section_path = {"grp", "child"}},
                            });

    auto schedule = nx::test_schedule::create(with_filter("child"), reg);
    CHECK(schedule.instances.size() == 1);
}

TEST("aliases - a full sweep (no filter) does not expand aliases")
{
    nx::test_registry reg;
    add_invocable(reg, "child", [&](int) { CHECK(true); });
    reg.add_declaration("drv", {}, [&] { nx::invoke_tests<int>("grp", 0); });

    nx::setup s(reg);
    s.define_alias("child", {nx::alias_fragment{.driver = s.find_test("drv"), .section_path = {"grp", "child"}}});

    // empty filters: the driver runs unscoped (invoking every invocable); the alias must not add a second run
    auto schedule = nx::test_schedule::create({}, reg);
    REQUIRE(schedule.instances.size() == 1);
    CHECK(schedule.instances[0].declaration == s.find_test("drv"));
    CHECK(schedule.instances[0].section_scopes.empty());
}

TEST("aliases - a driver selected directly by name subsumes its alias fragments (no double run)")
{
    int ran = 0;

    nx::test_registry reg;
    add_invocable(reg, "child",
                  [&](int)
                  {
                      ++ran;
                      CHECK(true);
                  });
    reg.add_declaration("drv", {}, [&] { nx::invoke_tests<int>("grp", 0); });

    nx::setup s(reg);
    // alias name contains "drv" so a "drv" filter hits both the driver (unscoped) and this alias (scoped)
    s.define_alias("drv-child", {nx::alias_fragment{.driver = s.find_test("drv"), .section_path = {"grp", "child"}}});

    auto schedule = nx::test_schedule::create(with_filter("drv"), reg);
    // only the unscoped driver run — the alias fragment is dropped as already covered by it
    REQUIRE(schedule.instances.size() == 1);
    CHECK(schedule.instances[0].section_scopes.empty());

    auto exec = nx::execute_tests(schedule, with_filter("drv"));
    CHECK(ran == 1); // "child" runs exactly once, not once per driver + once per fragment
}

TEST("aliases - a broad filter over drivers and aliases runs each invocable once per backend")
{
    int ran = 0;

    // Shape of the sg case: two backend drivers + one shared invocable, and an alias per invocable. A filter
    // ("sg") that hits both the driver names and the alias name must not run the invocable twice on a backend.
    nx::test_registry reg;
    add_invocable(reg, "sg thing",
                  [&](int)
                  {
                      ++ran;
                      CHECK(true);
                  });
    reg.add_declaration("sg X backend", {}, [&] { nx::invoke_tests<int>("x", 0); });
    reg.add_declaration("sg Y backend", {}, [&] { nx::invoke_tests<int>("y", 0); });

    nx::setup s(reg);
    s.define_alias("sg thing",
                   {
                       nx::alias_fragment{.driver = s.find_test("sg X backend"), .section_path = {"x", "sg thing"}},
                       nx::alias_fragment{.driver = s.find_test("sg Y backend"), .section_path = {"y", "sg thing"}},
                   });

    auto schedule = nx::test_schedule::create(with_filter("sg"), reg);
    // just the two unscoped driver runs; both alias fragments are subsumed by their name-matched drivers
    CHECK(schedule.instances.size() == 2);

    auto exec = nx::execute_tests(schedule, with_filter("sg"));
    CHECK(ran == 2); // once under each backend driver, never twice on the same one
}

TEST("aliases - a substring filter does not reach a manual driver through its alias")
{
    // Aliases are filters, not a bucket escape hatch: reaching a manual driver through one of its aliases takes
    // the alias's exact name, exactly as reaching the driver directly takes the driver's. Otherwise a bare
    // filter would open every window-opening backend an alias happens to name.
    int ran = 0;

    nx::test_registry reg;
    add_invocable(reg, "gpu thing",
                  [&](int)
                  {
                      ++ran;
                      CHECK(true);
                  });
    reg.add_declaration("gpu backend", nx::config::cfg{.bucket = nx::config::test_bucket::manual},
                        [&] { nx::invoke_tests<int>("g", 0); });

    nx::setup s(reg);
    s.define_alias("gpu thing",
                   {nx::alias_fragment{.driver = s.find_test("gpu backend"), .section_path = {"g", "gpu thing"}}});

    // "gpu" substring-matches both the alias and the driver name, but the driver is manual: nothing runs.
    {
        auto schedule = nx::test_schedule::create(with_filter("gpu"), reg);
        CHECK(schedule.instances.empty());
    }

    // The alias's exact name reaches it, scoped to the fragment.
    {
        auto schedule = nx::test_schedule::create(with_filter("gpu thing"), reg);
        REQUIRE(schedule.instances.size() == 1);
        CHECK(schedule.instances[0].declaration == s.find_test("gpu backend"));
        CHECK(schedule.instances[0].section_scopes.size() == 1);

        auto exec = nx::execute_tests(schedule, with_filter("gpu thing"));
        CHECK(ran == 1);
    }

    // So does --manual, which sweeps the bucket by substring.
    ran = 0;
    {
        auto config = with_filter("gpu");
        config.selected_bucket = nx::config::test_bucket::manual;
        config.allow_cross_bucket_naming = false;

        auto schedule = nx::test_schedule::create(config, reg);
        REQUIRE(schedule.instances.size() == 1); // the driver, unscoped (its own name matched)

        auto exec = nx::execute_tests(schedule, config);
        CHECK(ran == 1);
    }
}

// --- callback path: NX_TEST_SETUP registers a global callback that run_setup_callbacks drives against a
// registry. The callback below runs at real startup against the static registry (harmless: it only defines an
// alias if its driver exists) and, in the test, against a local registry that provides that driver.

TEST("aliases - alias-cb-driver")
{
    // A plain driver the callback below binds its alias to. Present in both the static and the local registry.
    CHECK(true);
}

NX_TEST_SETUP(nx::setup& s)
{
    if (auto const* d = s.find_test("aliases - alias-cb-driver"))
        s.define_alias("alias-cb-smoke", {nx::alias_fragment{.driver = d, .section_path = {"x"}}});
}

TEST("aliases - NX_TEST_SETUP callbacks run against the passed registry")
{
    nx::test_registry reg;
    reg.add_declaration("aliases - alias-cb-driver", {}, [&] {});

    nx::run_setup_callbacks(reg);

    bool found = false;
    for (auto const& alias : reg.aliases)
        if (alias.name == "alias-cb-smoke")
        {
            found = true;
            REQUIRE(alias.fragments.size() == 1);
            CHECK(alias.fragments[0].driver == reg.declarations.data());
        }
    CHECK(found);
}
