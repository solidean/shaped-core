#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <versioned-document/op_builder.hh>
#include <versioned-document/value_builder.hh>

using namespace cc::primitive_defines;

// vdoc's edit path is realtime — a one-entity edit is roughly ten microseconds — so WHERE a span sits is the whole
// question.
// The entry points carry one; the per-entity and per-property loops under them deliberately do not.
// A span that drifted down into one of those would cost more than the edit it measured.

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

isize count_scopes(cc::rec::recording const& r, cc::string_view name, cc::string_view expected_domain)
{
    isize n = 0;
    for (auto const& b : r.blocks())
    {
        auto const v = b.view();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (auto const e = *it; e.kind() == cc::rec::event_kind::scope_begin && cc::string_view(e.name()) == name)
            {
                CHECK(cc::string_view(e.domain()->name()) == expected_domain);
                ++n;
            }
    }
    return n;
}
} // namespace

TEST("vdoc/recording - building an op is scoped under vdoc's domain", nx::config::exclusive(), nx::config::owns_recorder)
{
    rec_fixture const fixture;

    cc::rec::recording_listener rl;
    auto const handle = cc::rec::register_listener(rl);

    auto graph = vdoc::op_graph();
    auto const o = vdoc::op_builder()
                       .set_raw(vdoc::property_path{.entity = vdoc::entity_id::of("e1"),
                                                    .component = vdoc::component_type_id::of("T"),
                                                    .property = vdoc::property_id::of("x")},
                                vdoc::value::of(1))
                       .build(graph);
    (void)o;

    cc::rec::flush_blocking();
    cc::rec::unregister_listener(handle);

    auto const r = rl.take();

    // One span per build, and attributed to vdoc rather than to the default domain.
    CHECK(count_scopes(r, "vdoc.op_builder.build", "vdoc") == 1);
}
