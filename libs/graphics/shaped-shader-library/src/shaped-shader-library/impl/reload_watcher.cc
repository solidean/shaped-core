#include <clean-core/common/assert.hh>
#include <clean-core/container/set.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/impl/reload_watcher.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

// TEMPORARY: there is no cc:: sleep and cc::threaded_actor has no timed wait, so the poll interval is a
// std::this_thread::sleep_for here. A cc::sleep_for (or a wait_for hook on the actor) would replace both
// this include and the slicing below. Only the polling fallback needs it — a watched reload parks on the
// mailbox instead, which shutdown already wakes.
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

bool same_paths(cc::vector<cc::string> const& a, cc::vector<cc::string> const& b)
{
    if (a.size() != b.size())
        return false;
    for (cc::isize i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

void slib::impl::reload_wake::arm(cc::threaded_actor<check_now>* actor)
{
    CC_ASSERT(actor != nullptr, "cannot arm a wake without an actor");

    _actor.lock([&](cc::threaded_actor<check_now>*& a) { a = actor; });

    // The watcher's first scan ran in its constructor, before this actor existed, so any wake it asked for
    // was dropped. Ask once more now that there is somewhere to ask — which also covers the window between
    // that scan reading revisions and its watches being registered.
    _scan_pending.store(false);
    fire();
}

void slib::impl::reload_wake::disarm()
{
    _actor.lock([](cc::threaded_actor<check_now>*& a) { a = nullptr; });
}

void slib::impl::reload_wake::fire()
{
    if (_scan_pending.exchange(true))
        return; // a scan is already asked for: several mounts firing on one save is still one scan

    _actor.lock(
        [&](cc::threaded_actor<check_now>* actor)
        {
            // Not armed yet, or the actor is shutting down and rejected the message. Hand the latch back,
            // or this fire would swallow every later one.
            if (actor == nullptr || !actor->enqueue_message(check_now{}))
                _scan_pending.store(false);
        });
}

void slib::impl::reload_wake::clear_pending()
{
    _scan_pending.store(false);
}

slib::impl::reload_watcher::reload_watcher(shader_library& library,
                                           double interval_ms,
                                           bool threaded,
                                           bool force_polling,
                                           std::shared_ptr<cc::atomic<bool>> stopping,
                                           std::shared_ptr<reload_wake> wake)
  : _library(&library),
    _interval_ms(interval_ms),
    _threaded(threaded),
    _polling(force_polling),
    _stopping(cc::move(stopping)),
    _wake(cc::move(wake))
{
    // Record where every watched file stands right now. A scan treats an unseen path as new and only seeds
    // it, so without this first pass the very next edit would be the one seeded — and missed. Runs on the
    // caller, before the actor starts, so "changed" means "changed since hot reload began".
    scan();
}

void slib::impl::reload_watcher::on_message(check_now)
{
    // Before the scan, not after: a change landing while we scan has to re-arm, rather than be swallowed
    // by the pass that was already too far along to see it.
    _wake->clear_pending();
    scan();
}

bool slib::impl::reload_watcher::on_process()
{
    if (!_threaded)
    {
        // Unthreaded: this runs on whoever called poll_hot_reload(), so it must not sleep. While a watch is
        // live the mailbox is the whole trigger and a poll with nothing to do costs nothing; polling, the
        // caller's own cadence is the interval.
        if (_polling)
            scan();
        return false;
    }

    // Watching: park on the mailbox until a filesystem — or a first acquire — wakes us. Idle costs nothing,
    // which is the entire point of the exercise.
    if (!_polling)
        return false;

    // Nothing will ever notify us, so the loop is the sleep.
    if (!sleep_unless_stopping(_interval_ms, *_stopping))
        return false;

    scan();
    return true;
}

void slib::impl::reload_watcher::on_thread_shutdown()
{
    // Drop the watches while everything they point at is still alive. Each subscription's destructor
    // guarantees its sink has finished, so once this returns nothing can wake a watcher that is going away.
    _subscriptions.clear();
    _watched_dirs.clear();
}

bool slib::impl::reload_watcher::note_revision(cc::string_view path)
{
    auto const revision = _library->filesystem().revision(path);

    auto* const known = _revisions.get_ptr(path);
    if (known == nullptr)
    {
        // First time we have seen this path — an include a compile just discovered. Seed it; treating it as
        // changed would reload the shader that only now read it.
        _revisions[path] = revision;
        return false;
    }

    if (*known == revision)
        return false;

    *known = revision;
    return true;
}

void slib::impl::reload_watcher::seed_revision(cc::string_view path)
{
    if (_revisions.get_ptr(path) == nullptr)
        _revisions[path] = _library->filesystem().revision(path);
}

void slib::impl::reload_watcher::scan()
{
    auto const assets = _library->assets();

    // Collect every change first: staging one asset's reload rewrites its dependency list, which would
    // otherwise hide a trigger for the asset scanned after it.
    cc::set<cc::string> changed;
    for (auto const& asset : assets)
        for (auto const& dependency : asset->dependencies())
            if (note_revision(dependency))
                changed.insert(dependency);

    bool staged = false;
    if (!changed.empty())
    {
        for (auto const& asset : assets)
        {
            bool affected = false;
            for (auto const& dependency : asset->dependencies())
                if (changed.contains(dependency))
                    affected = true;

            // Staging only: the recompile runs async and the next acquire() promotes it. The watcher never
            // touches the shader a consumer is currently using.
            if (affected)
            {
                asset->stage_reload();
                staged = true;
            }
        }
    }

    // Staging resolved the includes afresh, so the dependency set has already moved — stage_reload records
    // it before it returns. Seed whatever appeared: watching has no next tick to catch it on, so an include
    // discovered here would otherwise read as changed the first time anything else woke us.
    if (staged)
        for (auto const& asset : assets)
            for (auto const& dependency : asset->dependencies())
                seed_revision(dependency);

    // The directories we depend on can have moved — a newly discovered include, or a first acquire that
    // only now gave an asset any dependencies at all.
    resubscribe();
}

cc::vector<cc::string> slib::impl::reload_watcher::dependency_dirs() const
{
    cc::vector<cc::string> dirs;
    for (auto const& asset : _library->assets())
    {
        for (auto const& dependency : asset->dependencies())
        {
            auto const dir = parent_path(dependency);

            // A handful of directories for any number of shaders, so a linear scan is the whole dedup.
            bool known = false;
            for (auto const& d : dirs)
                if (d == dir)
                    known = true;

            if (!known)
                dirs.push_back(cc::string::create_copy_of(dir));
        }
    }
    return dirs;
}

void slib::impl::reload_watcher::resubscribe()
{
    if (_polling)
        return;

    auto dirs = dependency_dirs();
    if (same_paths(dirs, _watched_dirs))
        return;

    // Per directory, deduplicated, rather than per file or once at the root: a real_filesystem coalesces a
    // file watch into a directory watch anyway, and watching the root of a rooted one means watching an
    // entire source tree.
    //
    // Drop the old set first — each destructor guarantees its sink is done, so nothing from the previous
    // watches can land in the middle of this.
    _subscriptions.clear();
    _watched_dirs.clear();

    auto const& fs = _library->filesystem();
    for (auto const& dir : dirs)
    {
        auto sub = fs.watch(dir, [wake = _wake] { wake->fire(); });
        if (!sub.has_value())
        {
            // Something under this directory cannot notify. Poll everything rather than watch part of the
            // tree and quietly miss the rest.
            _subscriptions.clear();
            _polling = true;
            return;
        }

        _subscriptions.push_back(cc::move(sub.value()));
    }

    _watched_dirs = cc::move(dirs);

    // An edit that landed while we were rebuilding would have fired a watch we had already dropped. One
    // extra scan is cheap next to a missed reload, and it settles: the next pass finds the same directories
    // and stops above.
    _wake->fire();
}
