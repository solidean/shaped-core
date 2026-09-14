#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

// The run seed: what every test's seed derives from, and what shuffles the schedule and each invocation's children.
// Every test here nests a run, so every test is no_scheduler.

using namespace cc::primitive_defines;

namespace
{
nx::test_schedule_config shuffled(u64 seed, int jobs = 1)
{
    auto config = nx::test_schedule_config{};
    config.jobs = jobs;
    config.seed = seed;
    config.shuffle = true;
    return config;
}

struct seed_key
{
    cc::vector<cc::string>* order = nullptr;
};

/// One string per order, since cc::vector has no equality of its own.
cc::string joined(cc::vector<cc::string> const& names)
{
    auto out = cc::string();
    for (auto const& n : names)
        out += n + ",";
    return out;
}
} // namespace

TEST("seed - a test's seed comes from the run seed and its name, and a pin wins", no_scheduler)
{
    auto const seeds_of = [](u64 run_seed)
    {
        auto seen = cc::vector<u64>::create_filled(3, u64(0));
        nx::test_registry reg;
        reg.add_declaration("alpha", {}, [&] { seen[0] = nx::test_seed(), CHECK(true); });
        reg.add_declaration("beta", {}, [&] { seen[1] = nx::test_seed(), CHECK(true); });
        reg.add_declaration("pinned", nx::impl::merge_config(nx::config::seed(9)),
                            [&] { seen[2] = nx::test_seed(), CHECK(true); });
        auto const schedule = nx::test_schedule::create({}, reg);
        (void)nx::execute_tests(schedule, shuffled(run_seed));
        return seen;
    };

    auto const first = seeds_of(1234);
    auto const again = seeds_of(1234);
    auto const other = seeds_of(99);

    CHECK(first[0] == again[0]); // the same run seed, the same test seed, whatever order the shuffle chose
    CHECK(first[1] == again[1]);
    CHECK(first[0] != first[1]); // named apart
    CHECK(first[0] != other[0]); // and the run seed matters
    CHECK(first[2] == 9);        // a pin is a pin
    CHECK(other[2] == 9);
}

TEST("seed - a filtered re-run hands a test the seed it had in the full run", no_scheduler)
{
    auto const seed_of_beta = [](cc::vector<cc::string> filters)
    {
        auto seed = u64(0);
        nx::test_registry reg;
        reg.add_declaration("alpha", {}, [] { CHECK(true); });
        reg.add_declaration("beta", {}, [&] { seed = nx::test_seed(), CHECK(true); });
        reg.add_declaration("gamma", {}, [] { CHECK(true); });
        auto select = nx::test_schedule_config{};
        select.filters = cc::move(filters);
        auto const schedule = nx::test_schedule::create(select, reg);
        (void)nx::execute_tests(schedule, shuffled(77));
        return seed;
    };

    CHECK(seed_of_beta({}) == seed_of_beta({"beta"}));
}

TEST("seed - shuffling permutes the run order by the seed, and reports keep schedule order", no_scheduler)
{
    auto const run_order = [](u64 seed)
    {
        auto order = cc::vector<cc::string>();
        nx::test_registry reg;
        for (auto i = 0; i < 16; ++i)
            reg.add_declaration(cc::format("t{:02}", i), {},
                                [&order, i]
                                {
                                    order.push_back(cc::format("t{:02}", i));
                                    CHECK(true);
                                });
        auto const schedule = nx::test_schedule::create({}, reg);
        auto const exec = nx::execute_tests(schedule, shuffled(seed));

        REQUIRE(exec.executions.size() == 16);
        for (auto i = 0; i < 16; ++i)
            CHECK(exec.executions[i].instance.declaration->name == cc::format("t{:02}", i));
        return joined(order);
    };

    auto const a = run_order(5);
    auto const b = run_order(5);
    auto const c = run_order(6);
    CHECK(a == b);
    CHECK(a != c); // 16 tests: two seeds agreeing on the whole order would be a one-in-20-trillion coincidence
}

TEST("seed - an invocation shuffles its children by the driver's seed", no_scheduler)
{
    auto const child_order = [](u64 seed)
    {
        auto order = cc::vector<cc::string>();
        nx::test_registry reg;
        for (auto i = 0; i < 12; ++i)
        {
            using sig = cc::signature<void(seed_key)>;
            reg.add_invocable_declaration(cc::format("child {:02}", i), {}, cc::arg_types_of(sig{}),
                                          [i](cc::span<nx::typed_value*> in)
                                          {
                                              in[0]->get<seed_key>().order->push_back(cc::format("child {:02}", i));
                                              CHECK(true);
                                          });
        }
        reg.add_declaration("driver", {}, [&] { nx::invoke_tests("g", seed_key{&order}); });
        auto const schedule = nx::test_schedule::create({}, reg);
        auto const exec = nx::execute_tests(schedule, shuffled(seed));
        CHECK(exec.count_failed_tests() == 0);
        REQUIRE(order.size() == 12);
        return joined(order);
    };

    auto const a = child_order(11);
    auto const b = child_order(11);
    auto const c = child_order(12);
    CHECK(a == b);
    CHECK(a != c);
}

TEST("seed - a real run shuffles by a clock seed, and --seed pins it")
{
    char arg0[] = "nexus";
    char seed_flag[] = "--seed";
    char seed_value[] = "42";

    char* bare[] = {arg0};
    auto const drawn = nx::test_schedule_config::create_from_args(1, bare);
    CHECK(drawn.shuffle);

    char* pinned_argv[] = {arg0, seed_flag, seed_value};
    auto const pinned = nx::test_schedule_config::create_from_args(3, pinned_argv);
    CHECK(pinned.shuffle);
    CHECK(pinned.seed == 42);

    // A hand-built config keeps schedule order, for the meta-tests asserting on it.
    CHECK(!nx::test_schedule_config{}.shuffle);
}
