#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-shader-library/filesystem/watch.hh>
#include <shaped-shader-library/fwd.hh>

#include <memory>

/// The per-OS seam behind real_filesystem::watch. One platform implements create_watch_backend(); the rest
/// get the null fallback, which is what makes real_filesystem answer "I cannot notify — poll me".

namespace slib::impl
{
/// Watches directories on the real disk. One per real_filesystem, built on its first watch(), and it owns
/// whatever threads and OS handles that takes.
class watch_backend
{
public:
    virtual ~watch_backend() = default;

    /// Watches `native_dir` (an absolute native path) for changes to anything directly inside it — not its
    /// subdirectories, since the caller subscribes per directory anyway.
    ///
    /// nullopt if the OS refused, a directory that does not exist being the ordinary case. The subscription
    /// must not outlive this backend.
    [[nodiscard]] virtual cc::optional<watch_subscription> watch_dir(cc::string_view native_dir, watch_sink sink) = 0;
};

/// This platform's backend, or nullptr where there is none — real_filesystem then reports that it cannot
/// notify and its caller keeps polling. Also nullptr without threads: a backend needs one to wait on.
[[nodiscard]] std::unique_ptr<watch_backend> create_watch_backend();
} // namespace slib::impl
