#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shaped-graphics/context/metrics.hh>

using namespace cc::primitive_defines;

// advance_epoch records each stat's change over the epoch into cc::rec; this pins what that recording says.

namespace
{
/// Declared first in a test so it is destroyed last: a recording holds chunk references.
struct rec_fixture
{
    rec_fixture()
    {
        auto cfg = cc::rec::config{};
        cfg.threaded = false;
        cfg.overflow = cc::rec::overflow_policy::grow_unbounded;
        cc::rec::initialize(cfg);
    }
    ~rec_fixture() { cc::rec::shutdown(); }

    rec_fixture(rec_fixture const&) = delete;
    rec_fixture& operator=(rec_fixture const&) = delete;
};

/// The sum of every accumulate named `name`, or -1 when there is none.
f64 accumulated(cc::rec::recording const& r, cc::string_view name)
{
    auto total = -1.0;
    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; e.kind() == cc::rec::event_kind::stat_accumulate && cc::string_view(e.name()) == name)
                total = (total < 0 ? 0 : total) + e.field_as_double("value").value_or(0);
    }
    return total;
}
} // namespace

TEST("sg/stats - a snapshot difference is per stat, and keeps what the backend counts")
{
    auto totals = sg::impl::stat_totals();
    totals.set_counted(sg::all_stats.without(sg::barrier_stats));
    auto const before = totals.snapshot();

    auto counts = sg::impl::stat_counts();
    counts.add(sg::stat::draws, 3);
    counts.add(sg::stat::bytes_uploaded_inline, 100);
    totals.fold(counts);
    totals.add(sg::stat::draws);

    auto const d = totals.snapshot() - before;
    CHECK(d[sg::stat::draws] == 4);
    CHECK(d[sg::stat::bytes_uploaded_inline] == 100);
    CHECK(d[sg::stat::dispatches] == 0);
    CHECK(d.is_counted(sg::stat::draws));
    CHECK(!d.is_counted(sg::stat::buffer_barriers));
}

TEST("sg/stats - every stat has a recorded name and a unit")
{
    for (auto i = 0; i < sg::stat_count; ++i)
    {
        auto const info = sg::info(sg::stat(i));
        CHECK(info.name.starts_with("sg."));
        CHECK(info.unit != nullptr);
    }
    CHECK(sg::info(sg::stat::gpu_wait_nanoseconds).unit == &cc::rec::unit_nanoseconds);
}

TEST("sg/stats - a recorded epoch is one accumulate per stat that moved, and none for an uncounted one",
     nx::config::exclusive(),
     nx::config::owns_recorder)
{
    rec_fixture const fixture;
    cc::rec::recording_listener rl;
    auto const handle = cc::rec::register_listener(rl);

    auto totals = sg::impl::stat_totals();
    totals.set_counted(sg::all_stats.without(sg::barrier_stats));
    auto const before = totals.snapshot();
    totals.add(sg::stat::draws, 7);
    totals.add(sg::stat::buffer_barriers, 2); // uncounted: a backend that cannot see barriers never adds one, but if it did
    sg::impl::record_stats(totals.snapshot() - before);

    cc::rec::flush_blocking();
    cc::rec::unregister_listener(handle);
    auto const r = rl.take();

    CHECK(accumulated(r, "sg.draws") == 7.0);
    CHECK(accumulated(r, "sg.dispatches") == -1.0);
    CHECK(accumulated(r, "sg.buffer_barriers") == -1.0);
}
