#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-shader-library/filesystem/filesystem.hh>

namespace slib
{
/// A filesystem assembled from other filesystems mounted at virtual paths. This is how slib addresses
/// shader sources without naming a disk layout anywhere: a package's folder, a shared include library,
/// and the binary's embedded copy all answer under paths the mounting code picked.
///
/// Among the mounts whose prefix matches a path, lookup goes **longest prefix first, then most recently
/// mounted first**, and the first mount holding the file answers. Two consequences worth knowing:
/// a mount at "common" beats one at the root for "common/brdf.hlsli", and re-mounting a prefix shadows
/// what was there — which is all an overlay is (mount the embedded copy, then the source folder over it).
///
/// Mounting is thread-safe against lookups, so a filesystem may be mounted while the reload watcher runs.
class mount_table final : public filesystem
{
public:
    [[nodiscard]] cc::optional<cc::string> read_text(cc::string_view path) const override;
    [[nodiscard]] file_revision revision(cc::string_view path) const override;

    /// Mounts `fs` so its own root answers `virtual_dir` (empty = the table's root). `fs` must not be
    /// null, and `virtual_dir` must not escape the root.
    void mount(cc::string_view virtual_dir, filesystem_handle fs);

    [[nodiscard]] isize mount_count() const;

private:
    struct entry
    {
        cc::string prefix; ///< normalized, no trailing '/'
        filesystem_handle fs;
        u64 salt = 0; ///< identifies this mount; folded into the revisions it serves (see revision())
    };

    /// One mount that could serve a path, with the path rebased onto that mount's root.
    struct candidate
    {
        cc::string_view path; ///< borrowed from the caller's normalized path
        filesystem_handle fs;
        u64 salt = 0;
    };

    struct state
    {
        cc::vector<entry> mounts; ///< kept in resolution order by mount(), so lookup is a plain scan
        u64 next_salt = 1;
    };

    /// The mounts that could serve `path` (already normalized), in resolution order. Copied out so the
    /// lock is not held across a read — real_filesystem hits the disk.
    [[nodiscard]] cc::vector<candidate> candidates_for(cc::string_view path) const;

    // Mutable so const reads can lock.
    mutable cc::mutex<state> _state;
};
} // namespace slib
