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

    /// Composes the mounts' own watches: every mount intersecting `prefix` is subscribed to, and the
    /// returned subscription owns all of them.
    ///
    /// All-or-nothing — if any intersecting mount cannot notify, this returns nullopt and the caller polls
    /// everything. Watching what we can and polling only the rest is a real refinement, deferred in
    /// libs/graphics/shaped-shader-library/docs/structure.md.
    ///
    /// A mount added while a watch is live is not picked up. In practice mounts stop moving when hot
    /// reload starts, which is what makes that affordable.
    [[nodiscard]] cc::optional<watch_subscription> watch(cc::string_view prefix, watch_sink sink) const override;

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

    /// One mount a watch has to reach, with the watched prefix rebased onto that mount's own root. Owned
    /// where a candidate's path is borrowed: a watch outlives the call that registered it.
    struct watch_target
    {
        cc::string prefix;
        filesystem_handle fs;
    };

    /// The mounts that could serve `path` (already normalized), in resolution order. Copied out so the
    /// lock is not held across a read — real_filesystem hits the disk.
    [[nodiscard]] cc::vector<candidate> candidates_for(cc::string_view path) const;

    /// The mounts intersecting `prefix` (already normalized), in resolution order.
    [[nodiscard]] cc::vector<watch_target> watch_targets_for(cc::string_view prefix) const;

    // Mutable so const reads can lock.
    mutable cc::mutex<state> _state;
};
} // namespace slib
