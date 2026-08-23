#include <clean-core/container/vector.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>

using namespace cc::primitive_defines;

// sg's own warnings used to be cc::eprintln, which ../../docs/testing.md called out as the one thing its tests could not
// cover: "that wants a real cc log system, which does not exist yet".
// It exists now, so this is that coverage — a warning is an EVENT, and an event can be asserted on.

namespace
{
/// Brings the recorder up for one test and takes it down again.
/// Declared FIRST in a test so it is destroyed LAST — a recording holds chunk references, and shutting the pool down
/// underneath one is a use-after-free rather than a diagnostic.
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

isize count_warnings(cc::rec::recording const& r, cc::string_view needle)
{
    isize n = 0;
    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; e.kind() == cc::rec::event_kind::log && e.level() == cc::rec::level::warning)
                if (e.payload_as_text().contains(needle) || cc::string_view(e.name()).contains(needle))
                {
                    CHECK(cc::string_view(e.domain()->name()) == "sg");
                    ++n;
                }
    }
    return n;
}

f64 last_stat(cc::rec::recording const& r, cc::string_view name)
{
    auto value = -1.0;
    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; e.kind() == cc::rec::event_kind::stat_snapshot && cc::string_view(e.name()) == name)
                value = e.field_as_double("value").value_or(-1.0);
    }
    return value;
}

} // namespace

TEST("sg/recording - the concurrent-command-list warning is an assertable event",
     nx::config::exclusive(),
     nx::config::owns_recorder)
{
    rec_fixture const fixture;

    cc::rec::recording_listener rl;
    auto const handle = cc::rec::register_listener(rl);

    {
        sg::command_list_slot_allocator alloc;
        cc::vector<sg::command_list_slot> slots;
        for (auto i = 0; i < 65; ++i)
            slots.push_back(alloc.acquire());

        for (auto const s : slots)
            (void)alloc.release(s);
    }

    cc::rec::flush_blocking();
    cc::rec::unregister_listener(handle);

    auto const r = rl.take();

    // Warned once past 64, and only once — the allocator's own guard, now visible to a test rather than to a terminal.
    CHECK(count_warnings(r, "more than 64 concurrent command lists") == 1);

    // The live count was already tracked to size a bitmask; nothing ever reported it until now.
    // Every slot was released, so the last reading is zero.
    CHECK(last_stat(r, "sg.command_lists.live") == 0.0);
}
