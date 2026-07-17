#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <shaped-shader-library/filesystem/impl/path.hh>
#include <shaped-shader-library/filesystem/impl/watch_registry.hh>
#include <shaped-shader-library/filesystem/mount_table.hh>

#include <memory>

namespace
{
/// One watch over several mounts. The child sinks all fire the same slot, so cancelling that one slot is
/// the whole teardown — the children are dropped afterwards only to release the OS watches they hold.
struct mount_subscription final : slib::watch_subscription::impl_base
{
    mount_subscription(std::shared_ptr<slib::impl::watch_slot> fan_in, cc::vector<slib::watch_subscription> children)
      : fan_in(cc::move(fan_in)), children(cc::move(children))
    {
    }

    ~mount_subscription() override
    {
        // Silence the caller's sink first, so an in-flight child notification cannot reach it while we
        // are still tearing down. Each child then guarantees the same for its own sink as it goes.
        fan_in->cancel();
        children.clear();
    }

    std::shared_ptr<slib::impl::watch_slot> fan_in;
    cc::vector<slib::watch_subscription> children;
};

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

cc::vector<slib::mount_table::watch_target> slib::mount_table::watch_targets_for(cc::string_view prefix) const
{
    cc::vector<watch_target> result;
    _state.lock(
        [&](state const& s)
        {
            for (auto const& mount : s.mounts)
            {
                // A mount and a watched prefix can meet two ways, and they rebase in opposite directions.
                // Note a lookup only ever asks the first question: it names one file, so a mount *below*
                // that file cannot answer it. A watch is the other way round — a mount below the prefix is
                // precisely one that can serve a change under it, and missing those is how a composed
                // watch silently stops seeing half the tree.
                if (impl::is_path_under(prefix, mount.prefix))
                    result.push_back(
                        watch_target{.prefix = cc::string::create_copy_of(impl::relative_to(prefix, mount.prefix)),
                                     .fs = mount.fs});
                else if (impl::is_path_under(mount.prefix, prefix))
                    result.push_back(watch_target{.prefix = cc::string(), .fs = mount.fs}); // its whole root
            }
        });
    return result;
}

cc::optional<slib::watch_subscription> slib::mount_table::watch(cc::string_view prefix, watch_sink sink) const
{
    auto const normalized = impl::normalize_path(prefix);
    if (!normalized.has_value())
        return watch_subscription(); // nothing is reachable under a prefix that escapes the root

    // One slot every child fires. Coalescing the N notifications one edit can produce is the caller's job;
    // cancelling them is ours, and sharing a slot makes that a single act.
    auto fan_in = std::make_shared<impl::watch_slot>(cc::move(sink));

    cc::vector<watch_subscription> children;
    for (auto const& target : watch_targets_for(normalized.value()))
    {
        // Off the lock: real_filesystem sets up an OS watch in here.
        auto child = target.fs->watch(target.prefix, [fan_in] { fan_in->fire(); });
        if (!child.has_value())
            return cc::nullopt; // one mount that cannot notify leaves the whole table unwatchable

        children.push_back(cc::move(child.value()));
    }

    return watch_subscription(std::make_unique<mount_subscription>(cc::move(fan_in), cc::move(children)));
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
