#pragma once

#include <clean-core/thread/mutex.hh>
#include <shaped-shader-library/filesystem/filesystem.hh>

#include <memory>

namespace slib::impl
{
class watch_backend;
}

namespace slib
{
/// A filesystem over a real directory on disk, rooted at `root_dir`. Paths resolve beneath that root
/// and nowhere else — `..` cannot climb out. `revision` folds the file's modification time.
///
/// This — with its watch backends — is the **only** part of slib that touches the disk or the OS (see
/// docs/coding-guidelines.md). Everything else addresses shader sources through a mounted `filesystem`,
/// which is what lets a shipped build read embedded sources and a test read an in-memory one without
/// either knowing the difference.
///
/// A missing or unreadable root is not an error: every lookup simply finds nothing. That is what makes
/// "mount the source dir over the embedded copy, if it happens to exist" work without a mode flag.
class real_filesystem final : public filesystem
{
public:
    /// `root_dir` is an absolute native path. It need not exist.
    explicit real_filesystem(cc::string root_dir);
    ~real_filesystem() override;

    [[nodiscard]] cc::optional<cc::string> read_text(cc::string_view path) const override;
    [[nodiscard]] file_revision revision(cc::string_view path) const override;

    /// Asks the OS to watch the directory `prefix` names. nullopt where this platform has no watch backend,
    /// under SC_THREADS=OFF, or when the directory does not exist — a shipped build with no source tree
    /// being the ordinary case for the last one.
    ///
    /// A file prefix becomes a watch on its whole directory, so a sibling's change fires the sink too. The
    /// contract allows that on purpose; filtering here would only duplicate what revision() already settles.
    [[nodiscard]] cc::optional<watch_subscription> watch(cc::string_view prefix, watch_sink sink) const override;

    [[nodiscard]] cc::string_view root_dir() const { return _root_dir; }

private:
    /// `path` resolved against the root, or nullopt if it escapes. Native separators.
    [[nodiscard]] cc::optional<cc::string> to_native_path(cc::string_view path) const;

    struct watch_state
    {
        std::unique_ptr<impl::watch_backend> backend;
        bool created = false; ///< so a platform without a backend is asked once, not once per watch
    };

    cc::string _root_dir;

    /// The OS watcher, built on the first watch() and shared by every watch on this filesystem. Mutable so
    /// the const watch() can build it. Stays null where the platform has no backend, which is exactly what
    /// makes watch() answer "poll me".
    mutable cc::mutex<watch_state> _watch_state;
};
} // namespace slib
