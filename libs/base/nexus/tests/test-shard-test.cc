#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <nexus/tests/alias.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

// --shard K/N, pinned: the shards partition the leaves of a run, and a test stays in its shard.
// The leaves are the plain tests and the children of a driver an alias names, which runs in every shard.
// A local registry keeps the meta-run out of the static one, as test-alias-test.cc does.

using namespace cc::primitive_defines;

namespace
{
constexpr int k_plain = 20;
constexpr int k_children = 30;

struct shard_fixture
{
    nx::test_registry reg;
    cc::vector<cc::string> ran; // every leaf that ran, in order
    int driver_runs = 0;

    shard_fixture()
    {
        for (auto i = 0; i < k_plain; ++i)
        {
            auto name = cc::format("plain {}", i);
            reg.add_declaration(name, {},
                                [this, name]
                                {
                                    ran.push_back(name);
                                    CHECK(true);
                                });
        }
        for (auto i = 0; i < k_children; ++i)
        {
            auto name = cc::format("child {}", i);
            reg.add_invocable_declaration(name, {}, cc::arg_types_of(cc::signature_of<void (*)(int)>{}),
                                          [this, name](cc::span<nx::typed_value*>)
                                          {
                                              ran.push_back(name);
                                              CHECK(true);
                                          });
        }
        reg.add_declaration("drv", {},
                            [this]
                            {
                                ++driver_runs;
                                nx::invoke_tests<int>("grp", 0);
                            });

        // One alias is what makes "drv" a driver the schedule knows of before it runs.
        auto s = nx::setup(reg);
        s.define_alias("child 0", {nx::alias_fragment{.driver = s.find_test("drv"), .section_path = {"grp", "child 0"}}});
    }

    /// The leaves shard `k` of `n` runs, in run order.
    cc::vector<cc::string> run_shard(int k, int n)
    {
        ran.clear();
        auto config = nx::test_schedule_config();
        config.shard_index = k;
        config.shard_count = n;
        auto const schedule = nx::test_schedule::create(config, reg);
        (void)nx::execute_tests(schedule, config);
        return ran;
    }
};

bool holds(cc::span<cc::string const> names, cc::string_view name)
{
    for (auto const& n : names)
        if (cc::string_view(n) == name)
            return true;
    return false;
}
} // namespace

TEST("shard - the shards of a run hold every leaf exactly once, and the aliased driver runs in each", no_scheduler)
{
    auto f = shard_fixture();
    for (auto const n : {2, 3, 5})
    {
        auto counts = cc::vector<int>();
        counts.resize_to_defaulted(k_plain + k_children);
        f.driver_runs = 0;
        auto nonempty = 0;
        for (auto k = 0; k < n; ++k)
        {
            auto const ran = f.run_shard(k, n);
            nonempty += ran.empty() ? 0 : 1;
            for (auto i = 0; i < k_plain; ++i)
                counts[i] += holds(ran, cc::format("plain {}", i)) ? 1 : 0;
            for (auto i = 0; i < k_children; ++i)
                counts[k_plain + i] += holds(ran, cc::format("child {}", i)) ? 1 : 0;
        }
        for (auto i = 0; i < counts.size(); ++i)
            CHECK(counts[i] == 1).context(cc::format("leaf {} of {} shards", i, n));
        CHECK(f.driver_runs == n);
        CHECK(nonempty == n); // 50 leaves over at most 5 shards: an empty one is a hash that does not spread
    }
}

TEST("shard - a test lands in the same shard on every run", no_scheduler)
{
    auto f = shard_fixture();
    for (auto k = 0; k < 3; ++k)
    {
        auto const first = f.run_shard(k, 3);
        auto const second = f.run_shard(k, 3);
        REQUIRE(first.size() == second.size());
        for (auto const& name : first)
            CHECK(holds(second, name)).context(name);
    }

    // The key is the name alone, so a reader can work out a test's shard without running anything.
    auto config = nx::test_schedule_config();
    config.shard_count = 4;
    auto in = 0;
    for (auto k = 0; k < 4; ++k)
    {
        config.shard_index = k;
        in += config.in_shard("plain 7") ? 1 : 0;
    }
    CHECK(in == 1);
}

TEST("shard - one shard is the whole run, and the CLI reads K/N")
{
    auto whole = nx::test_schedule_config();
    CHECK(whole.in_shard("anything"));

    char const* good[] = {"nexus-test.exe", "--shard", "2/5"};
    auto const parsed = nx::test_schedule_config::create_from_args(3, const_cast<char**>(good));
    CHECK(!parsed.parse_failed);
    CHECK(parsed.shard_index == 2);
    CHECK(parsed.shard_count == 5);

    // Any other subset than the one asked for is the wrong answer, so a malformed shard fails the run.
    for (auto const* const bad : {"5/5", "1/0", "x/2", "3"})
    {
        char const* argv[] = {"nexus-test.exe", "--shard", bad};
        CHECK(nx::test_schedule_config::create_from_args(3, const_cast<char**>(argv)).parse_failed).context(bad);
    }
}
