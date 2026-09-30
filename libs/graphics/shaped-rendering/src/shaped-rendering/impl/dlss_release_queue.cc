#include <shaped-graphics/context/context.hh>
#include <shaped-rendering/impl/dlss_ngx.hh>
#include <shaped-rendering/impl/dlss_release_queue.hh>

// Always compiled, unlike the two seam implementations beside it: the queue is the same either way, and without the
// SDK it simply never holds anything — `dlss_create_feature` hands back null there, so nothing is ever parked.

namespace sr::impl
{
dlss_release_queue::~dlss_release_queue()
{
    _parked.lock(
        [](cc::vector<entry>& parked)
        {
            for (auto const& e : parked)
                dlss_release_feature(e.feature);
            parked.clear();
        });
}

void dlss_release_queue::retire(void* feature, sg::epoch not_before)
{
    if (feature == nullptr)
        return;
    _parked.lock([&](cc::vector<entry>& parked) { parked.push_back({.feature = feature, .not_before = not_before}); });
}

void dlss_release_queue::sweep(sg::context const& ctx)
{
    // Read once, outside the lock: it is a virtual call on the context, and holding the lock across it would put a
    // backend's own synchronization under ours.
    auto const completed = u64(ctx.completed_epoch());

    auto due = cc::vector<void*>();
    _parked.lock(
        [&](cc::vector<entry>& parked)
        {
            for (auto i = parked.size() - 1; i >= 0; --i)
            {
                if (u64(parked[i].not_before) > completed)
                    continue;
                due.push_back(parked[i].feature);
                parked.remove_at(i);
            }
        });

    // Released outside the lock: NGX frees device memory here, which is not something to do while another thread is
    // waiting to park a feature of its own.
    for (auto* const feature : due)
        dlss_release_feature(feature);
}
} // namespace sr::impl
