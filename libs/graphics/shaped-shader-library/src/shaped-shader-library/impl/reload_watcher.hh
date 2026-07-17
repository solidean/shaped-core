#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/threaded_actor.hh>
#include <shaped-shader-library/filesystem/filesystem.hh>
#include <shaped-shader-library/fwd.hh>

#include <memory>

/// The reload watcher: polls every file the library's assets were built from and stages a recompile for
/// each shader whose sources moved. Internal — drive it through slib::shader_library.

namespace slib::impl
{
/// Ask the watcher to scan now, rather than waiting out its interval.
struct check_now
{
};

/// Threaded: its own thread rescans every `interval_ms`. Unthreaded: nothing runs until
/// shader_library::poll_hot_reload() pumps it, which is what makes a reload test deterministic.
///
/// cc::threaded_actor is the repo's background-thread primitive, and its unthreaded mode is what keeps
/// one code path across SC_THREADS=OFF and WebAssembly.
class reload_watcher final : public cc::threaded_actor_impl<check_now>
{
public:
    reload_watcher(shader_library& library, double interval_ms, bool threaded, std::shared_ptr<cc::atomic<bool>> stopping);

protected:
    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "slib shader reload"; }

    void on_message(check_now) override;
    bool on_process() override;

private:
    /// Reads every watched path's revision, then stages a recompile on each asset that a changed file
    /// feeds. Newly discovered paths are seeded, never treated as changed.
    void scan();

    shader_library* _library;
    double _interval_ms;
    bool _threaded;

    /// Set by the library before shutdown so a sleeping scan loop gives up promptly. Shared because the
    /// actor owns this impl and hands it back only once it has stopped.
    std::shared_ptr<cc::atomic<bool>> _stopping;

    /// Virtual path -> the revision we last saw. Touched only on the actor thread (or the pumping
    /// thread, unthreaded), so it needs no lock.
    cc::map<cc::string, file_revision> _revisions;
};
} // namespace slib::impl
