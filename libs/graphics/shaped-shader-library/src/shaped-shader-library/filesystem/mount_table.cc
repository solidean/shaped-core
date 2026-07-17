#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/filesystem/mount_table.hh>

namespace
{
/// Folds a mount's salt into a revision it served. Without this, a file that starts being served by a
/// *different* mount — a source file appearing over an embedded one — could report the revision the old
/// mount happened to give it and read as unchanged. Never yields `none`, so an existing file stays existing.
slib::file_revision mix_revision(cc::u64 salt, slib::file_revision revision)
{
    auto const mixed = cc::make_hash_finalized(salt, cc::u64(revision));
    return slib::file_revision(mixed == 0 ? 1 : mixed);
}
} // namespace

void slib::mount_table::mount(cc::string_view virtual_dir, filesystem_handle fs)
{
    CC_ASSERT(fs != nullptr, "cannot mount a null filesystem");

    auto normalized = impl::normalize_path(virtual_dir);
    CC_ASSERT(normalized.has_value(), "mount point must not escape the root");

    _state.lock(
        [&](state& s)
        {
            // Insert ahead of every equal-or-shorter prefix. One rule keeps the vector in resolution
            // order: longest prefix first, and among equal lengths the newest first (so a later mount
            // at the same prefix shadows the earlier one).
            isize at = s.mounts.size();
            for (isize i = 0; i < s.mounts.size(); ++i)
            {
                if (s.mounts[i].prefix.size() <= normalized.value().size())
                {
                    at = i;
                    break;
                }
            }

            s.mounts.emplace_back();
            for (isize i = s.mounts.size() - 1; i > at; --i)
                s.mounts[i] = cc::move(s.mounts[i - 1]);
            s.mounts[at] = entry{.prefix = cc::move(normalized.value()), .fs = cc::move(fs), .salt = s.next_salt++};
        });
}

cc::isize slib::mount_table::mount_count() const
{
    return _state.lock([](state const& s) { return s.mounts.size(); });
}

cc::vector<slib::mount_table::candidate> slib::mount_table::candidates_for(cc::string_view path) const
{
    cc::vector<candidate> result;
    _state.lock(
        [&](state const& s)
        {
            for (auto const& mount : s.mounts)
                if (impl::is_path_under(path, mount.prefix))
                    result.push_back(
                        candidate{.path = impl::relative_to(path, mount.prefix), .fs = mount.fs, .salt = mount.salt});
        });
    return result;
}

cc::optional<cc::string> slib::mount_table::read_text(cc::string_view path) const
{
    auto const normalized = impl::normalize_path(path);
    if (!normalized.has_value())
        return cc::nullopt;
    auto const& resolved = normalized.value();

    for (auto const& c : candidates_for(resolved))
        if (auto text = c.fs->read_text(c.path); text.has_value())
            return text;
    return cc::nullopt;
}

slib::file_revision slib::mount_table::revision(cc::string_view path) const
{
    auto const normalized = impl::normalize_path(path);
    if (!normalized.has_value())
        return file_revision::none;
    auto const& resolved = normalized.value();

    // The first mount holding the file decides — the same one read_text takes, so a revision always
    // describes the content a read returns.
    for (auto const& c : candidates_for(resolved))
        if (auto const rev = c.fs->revision(c.path); rev != file_revision::none)
            return mix_revision(c.salt, rev);
    return file_revision::none;
}
