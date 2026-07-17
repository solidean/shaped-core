#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-shader-library/filesystem/filesystem.hh>
#include <shaped-shader-library/fwd.hh>

#include <memory>

/// The reload watcher: watches every file the library's assets were built from and stages a recompile for
/// each shader whose sources moved. Internal — drive it through slib::shader_library.

namespace slib::impl
{
/// Ask the watcher to scan now, rather than waiting out its interval.
struct check_now
{
};

/// How a filesystem notification reaches the watcher's mailbox.
///
/// Separate from the watcher, and shared, because the actor owns the impl: reload_watcher is built inside
/// make_threaded_actor and never handed back, so it cannot reach the actor it lives in. The library arms
/// this once the actor exists and before it starts.
class reload_wake final
{
public:
    /// Points the wake at its actor. Call before start(), so nothing can fire into a gap.
    void arm(cc::threaded_actor<check_now>* actor);

    /// Stops firing. Once this returns, no further message reaches the actor.
    void disarm();

    /// Asks for one scan. Coalesces — the several mounts and directories a single save touches must not
    /// become several scans. Safe from any thread and cheap: an OS watcher thread calls it.
    void fire();

    /// Clears the coalescing latch, re-arming fire(). The watcher calls this *before* it scans.
    void clear_pending();

private:
    cc::atomic<bool> _scan_pending{false};

    // Guarded rather than a bare pointer: disarm() has to mean an in-flight fire() has finished with the
    // actor, not merely that the next one will notice it is gone.
    cc::mutex<cc::threaded_actor<check_now>*> _actor{nullptr};
};

/// Watching: the filesystem wakes the mailbox and the actor is otherwise parked, costing nothing while
/// nobody edits. Polling: no filesystem could notify (or reload_config::force_polling asked for it), so
/// its own thread rescans every `interval_ms`. Unthreaded mirrors both, driven by
/// shader_library::poll_hot_reload() instead of a thread — which is what makes a reload test deterministic.
///
/// cc::threaded_actor is the repo's background-thread primitive, and its unthreaded mode is what keeps one
/// code path across SC_THREADS=OFF and WebAssembly.
class reload_watcher final : public cc::threaded_actor_impl<check_now>
{
public:
    reload_watcher(shader_library& library,
                   double interval_ms,
                   bool threaded,
                   bool force_polling,
                   std::shared_ptr<cc::atomic<bool>> stopping,
                   std::shared_ptr<reload_wake> wake);

protected:
    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "slib shader reload"; }

    void on_message(check_now) override;
    bool on_process() override;
    void on_thread_shutdown() override;

private:
    /// Reads every watched path's revision, stages a recompile on each asset a changed file feeds, then
    /// re-arms the watches. Newly discovered paths are seeded, never treated as changed.
    void scan();

    /// Compares `path` against the revision we last saw, returning whether it moved. An unseen path is
    /// seeded and reported unchanged.
    [[nodiscard]] bool note_revision(cc::string_view path);

    /// Records `path`'s revision only if it is new. Never reports a change — used after staging, where a
    /// path we have only now heard of was discovered by the compile that just read it.
    void seed_revision(cc::string_view path);

    /// The distinct parent directories of every current dependency, in a stable order.
    [[nodiscard]] cc::vector<cc::string> dependency_dirs() const;

    /// Rebuilds the watches to cover exactly the directories the dependencies now live in; a no-op while
    /// that set has not moved. Falls back to polling if any of them cannot be watched.
    void resubscribe();

    shader_library* _library;
    double _interval_ms;
    bool _threaded;

    /// Whether we rescan on a timer rather than on notification. Starts at reload_config::force_polling and
    /// latches on for good the moment a directory we depend on turns out to be unwatchable — polling sees
    /// every change anyway, so there is nothing to be gained by trying to climb back out. Actor thread only.
    bool _polling;

    /// Set by the library before shutdown so a sleeping poll loop gives up promptly. Shared because the
    /// actor owns this impl and hands it back only once it has stopped. Unused while watching — there the
    /// mailbox wait ends itself on shutdown.
    std::shared_ptr<cc::atomic<bool>> _stopping;

    /// What a filesystem notification fires. Shared with the library, which also fires it when an asset's
    /// dependency set moves on a thread we would never hear from.
    std::shared_ptr<reload_wake> _wake;

    /// Virtual path -> the revision we last saw. Touched only on the actor thread (or the pumping thread,
    /// unthreaded), so it needs no lock.
    cc::map<cc::string, file_revision> _revisions;

    /// The live watches, and the directories they cover. Same order, and compared to decide a rebuild.
    cc::vector<watch_subscription> _subscriptions;
    cc::vector<cc::string> _watched_dirs;
};
} // namespace slib::impl
