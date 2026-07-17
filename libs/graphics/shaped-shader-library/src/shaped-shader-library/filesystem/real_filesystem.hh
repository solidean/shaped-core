#pragma once

#include <shaped-shader-library/filesystem/filesystem.hh>

namespace slib
{
/// A filesystem over a real directory on disk, rooted at `root_dir`. Paths resolve beneath that root
/// and nowhere else — `..` cannot climb out. `revision` folds the file's modification time.
///
/// This is the **only** part of slib that touches the disk (see docs/coding-guidelines.md). Everything
/// else addresses shader sources through a mounted `filesystem`, which is what lets a shipped build read
/// embedded sources and a test read an in-memory one without either knowing the difference.
///
/// A missing or unreadable root is not an error: every lookup simply finds nothing. That is what makes
/// "mount the source dir over the embedded copy, if it happens to exist" work without a mode flag.
class real_filesystem final : public filesystem
{
public:
    /// `root_dir` is an absolute native path. It need not exist.
    explicit real_filesystem(cc::string root_dir) : _root_dir(cc::move(root_dir)) {}

    [[nodiscard]] cc::optional<cc::string> read_text(cc::string_view path) const override;
    [[nodiscard]] file_revision revision(cc::string_view path) const override;

    [[nodiscard]] cc::string_view root_dir() const { return _root_dir; }

private:
    /// `path` resolved against the root, or nullopt if it escapes. Native separators.
    [[nodiscard]] cc::optional<cc::string> to_native_path(cc::string_view path) const;

    cc::string _root_dir;
};
} // namespace slib
