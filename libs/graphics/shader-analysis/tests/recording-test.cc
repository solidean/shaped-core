#include "spirv-builder.hh"

#include <clean-core/record/domain.hh>
#include <clean-core/record/event_view.hh>
#include <clean-core/record/listener.hh>
#include <clean-core/record/recording.hh>
#include <clean-core/record/system.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shader-analysis/metrics.hh>

using namespace cc::primitive_defines;

// What shan records about its own work: one scope per analysis, in the `shan` domain.

namespace
{
/// Brings the recorder up for one test and takes it down again, whatever the body does.
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

TEST("shan - an analysis records one scope, in the shan domain", nx::config::exclusive(), nx::config::owns_recorder)
{
    rec_fixture const fixture;
    auto b = shan_test::impl::spirv_builder();
    shan_test::impl::declare_basics(b);
    auto const m = b.read();

    cc::rec::recording_listener rl;
    auto const handle = cc::rec::register_listener(rl);
    (void)shan::analyze(m);
    cc::rec::flush_blocking();
    cc::rec::unregister_listener(handle);

    auto const probe = probe_scopes(rl.take(), "shan.analyze");
    CHECK(probe.count == 1);
    CHECK(probe.domain == "shan");
}
