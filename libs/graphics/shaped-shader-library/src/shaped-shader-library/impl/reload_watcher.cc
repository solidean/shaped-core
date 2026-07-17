#include <clean-core/container/set.hh>
#include <shaped-shader-library/impl/reload_watcher.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

// TEMPORARY: there is no cc:: sleep and cc::threaded_actor has no timed wait, so the poll interval is a
// std::this_thread::sleep_for here. A cc::sleep_for (or a wait_for hook on the actor) would replace both
// this include and the slicing below.
#include <chrono>
#include <thread>

namespace
{
/// Sleeping in slices so shutdown does not have to wait out a whole poll interval. Returns false if a
/// stop was requested part-way through.
bool sleep_unless_stopping(double total_ms, cc::atomic<bool> const& stopping)
{
    constexpr double k_slice_ms = 5; // bounds shutdown latency without waking often enough to matter

    for (double slept = 0; slept < total_ms; slept += k_slice_ms)
    {
        if (stopping.load())
            return false;

        auto const slice = total_ms - slept < k_slice_ms ? total_ms - slept : k_slice_ms;
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(slice));
    }
    return !stopping.load();
}
} // namespace

slib::impl::reload_watcher::reload_watcher(shader_library& library,
                                           double interval_ms,
                                           bool threaded,
                                           std::shared_ptr<cc::atomic<bool>> stopping)
  : _library(&library), _interval_ms(interval_ms), _threaded(threaded), _stopping(cc::move(stopping))
{
    // Record where every watched file stands right now. A scan treats an unseen path as new and only
    // seeds it, so without this first pass the very next edit would be the one seeded — and missed.
    // Runs on the caller, before the actor starts, so "changed" means "changed since hot reload began".
    scan();
}

void slib::impl::reload_watcher::on_message(check_now)
{
    scan();
}

bool slib::impl::reload_watcher::on_process()
{
    if (!_threaded)
    {
        // Unthreaded: this runs on whoever called poll_hot_reload(), so it must not sleep. One scan per
        // pump, and the caller's own cadence is the interval.
        scan();
        return false;
    }

    // Nothing sends us messages, so returning false would park on the condition variable forever. The
    // loop is the sleep.
    if (!sleep_unless_stopping(_interval_ms, *_stopping))
        return false;

    scan();
    return true;
}

void slib::impl::reload_watcher::scan()
{
    auto const assets = _library->assets();
    auto const& fs = _library->filesystem();

    // Collect every change first: staging one asset's reload rewrites its dependency list, which would
    // otherwise hide a trigger for the asset scanned after it.
    cc::set<cc::string> changed;
    for (auto const& asset : assets)
    {
        for (auto const& dependency : asset->dependencies())
        {
            auto const revision = fs.revision(dependency);
            auto* const known = _revisions.get_ptr(dependency);
            if (known == nullptr)
            {
                // First time we have seen this path — an include a compile just discovered. Seed it;
                // treating it as changed would reload the shader that only now read it.
                _revisions[dependency] = revision;
            }
            else if (*known != revision)
            {
                *known = revision;
                changed.insert(dependency);
            }
        }
    }

    if (changed.empty())
        return;

    for (auto const& asset : assets)
    {
        bool affected = false;
        for (auto const& dependency : asset->dependencies())
            if (changed.contains(dependency))
                affected = true;

        // Staging only: the recompile runs async and the next acquire() promotes it. The watcher never
        // touches the shader a consumer is currently using.
        if (affected)
            asset->stage_reload();
    }
}
