#include <babel-data/data/json.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// What babel-data records about its own work.
//
// A scope nobody can find is a scope that will be deleted by the next person who touches the function, so the ones
// that earn their keep are pinned here — as is the sub-domain each is attributed to, which is what makes
// "show me only the JSON reader" possible at all.

namespace
{
/// Brings the recorder up for one test and takes it down again, whatever the body does.
///
/// Declared FIRST in a test so it is destroyed LAST: a recording holds CHUNK REFERENCES, and letting shutdown() free
/// the pool while one is still alive is a use-after-free into the heap rather than a diagnostic.
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

/// Counts scope_begin events with `name`, and reports which domain they came from.
struct scope_probe
{
    isize count = 0;
    cc::string domain;
};

scope_probe probe_scopes(cc::rec::recording const& r, cc::string_view name)
{
    scope_probe out;
    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; e.kind() == cc::rec::event_kind::scope_begin && cc::string_view(e.name()) == name)
            {
                ++out.count;
                out.domain = e.domain()->name();
            }
    }
    return out;
}
} // namespace

TEST("babel/recording - a json parse is scoped only once it is big enough",
     nx::config::exclusive(),
     nx::config::owns_recorder)
{
    rec_fixture const fixture;

    cc::rec::recording_listener rl;
    auto const handle = cc::rec::register_listener(rl);

    // Small: a value rather than a document, and gating it is the whole point of CC_RECORD_SCOPE_IF.
    (void)babel::json::read(cc::string_view("{\"a\":1}"));

    // Big: comfortably past the threshold, so it earns a span.
    auto big = cc::string("[");
    while (big.size() < 200 * 1024)
        big += "1,";
    big += "1]";
    (void)babel::json::read(cc::string_view(big));

    cc::rec::flush_blocking();
    cc::rec::unregister_listener(handle);

    auto const r = rl.take();
    auto const json = probe_scopes(r, "json.read");

    CHECK(json.count == 1);
    CHECK(json.domain == "babel.json"); // its own sub-domain, so it silences separately from the mesh readers
}
