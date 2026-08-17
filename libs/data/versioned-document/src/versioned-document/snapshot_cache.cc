#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/set.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/snapshot_cache.hh>

using namespace cc::primitive_defines;

vdoc::snapshot_document const* vdoc::snapshot_cache::find(op_id const& id)
{
    auto* const e = _entries.get_ptr(id);
    if (e == nullptr)
        return nullptr;

    e->last_used = ++_tick;
    return &e->doc;
}

void vdoc::snapshot_cache::install(op_id const& id, snapshot_document doc, bool pinned)
{
    _entries[id] = entry{.doc = cc::move(doc), .pinned = pinned, .last_used = ++_tick};
    impl_trim();
}

bool vdoc::snapshot_cache::erase(op_id const& id)
{
    return _entries.erase(id);
}

cc::optional<vdoc::snapshot_document> vdoc::snapshot_cache::take(op_id const& id)
{
    auto* const e = _entries.get_ptr(id);
    if (e == nullptr)
        return {};

    auto doc = cc::move(e->doc);
    _entries.erase(id);
    return doc;
}

void vdoc::snapshot_cache::clear_unpinned()
{
    auto doomed = cc::vector<op_id>();
    for (auto const& [id, e] : _entries)
        if (!e.pinned)
            doomed.push_back(id);

    for (auto const& id : doomed)
        _entries.erase(id);
}

void vdoc::snapshot_cache::clear()
{
    _entries.clear();
}

bool vdoc::snapshot_cache::unpin(op_id const& id)
{
    auto* const e = _entries.get_ptr(id);
    if (e == nullptr)
        return false;

    e->pinned = false;

    // The budget was allowed to be exceeded while this entry was pinned, so releasing it is where that debt is paid.
    impl_trim();
    return true;
}

bool vdoc::snapshot_cache::is_pinned(op_id const& id) const
{
    auto const* const e = _entries.get_ptr(id);
    return e != nullptr && e->pinned;
}

isize vdoc::snapshot_cache::pinned_count() const
{
    auto n = isize(0);
    for (auto const& [id, e] : _entries)
        n += e.pinned ? 1 : 0;

    return n;
}

isize vdoc::snapshot_cache::owned_byte_size() const
{
    auto total = isize(0);
    for (auto const& [id, e] : _entries)
        total += e.doc.owned_byte_size();

    return total;
}

void vdoc::snapshot_cache::impl_trim()
{
    // Counted once rather than re-scanned per iteration: a pin cannot appear or vanish inside this loop, and only
    // unpinned entries are erased.
    auto unpinned = _entries.size() - pinned_count();

    while (unpinned > _budget.max_unpinned_entries)
    {
        --unpinned;
        auto oldest = op_id();
        auto oldest_tick = u64(0);
        auto found = false;

        for (auto const& [id, e] : _entries)
        {
            if (e.pinned)
                continue;
            if (!found || e.last_used < oldest_tick)
            {
                oldest = id;
                oldest_tick = e.last_used;
                found = true;
            }
        }

        // only pinned entries left, so the budget cannot be met and must not be enforced
        if (!found)
            return;

        _entries.erase(oldest);
    }
}

bool vdoc::install_snapshot(op_graph const& graph, op_id const& head, snapshot_cache& cache)
{
    if (!graph.contains(head))
        return false;

    // Unfiltered and single-head, which is what makes the result surviving(head) rather than a projection of it.
    cache.install(head, snapshot_document::create_owning_copy(graph.materialize(head, cache)));
    return true;
}

bool vdoc::install_snapshot_if_useful(op_graph const& graph, op_id const& head, snapshot_cache& cache, snapshot_policy policy)
{
    if (!graph.contains(head) || cache.contains(head))
        return false;

    // The probe walks back exactly as the sweep would, and stops the moment the answer is decided — so asking costs
    // at most min_ops_behind ops, however long the history behind the head actually is.
    auto seen = cc::set<op_id>();
    auto stack = cc::vector<op_id>();
    stack.push_back(head);
    auto walked = isize(0);

    while (!stack.empty() && walked < policy.min_ops_behind)
    {
        auto const id = stack.back();
        stack.remove_back();

        if (seen.contains(id))
            continue;

        auto const* const o = graph.find(id);
        if (o == nullptr)
            continue;

        seen.insert(id);
        ++walked;

        // a cached snapshot ends this branch, exactly as it would end the sweep's walk
        if (cache.contains(id))
            continue;

        for (auto const& parent : o->parents)
            stack.push_back(parent);
    }

    if (walked < policy.min_ops_behind)
        return false;

    return install_snapshot(graph, head, cache);
}

bool vdoc::advance_snapshot(op_graph const& graph, snapshot_cache& cache, op_id const& parent, op_id const& child)
{
    auto const* const o = graph.find(child);

    // A skeleton is refused rather than treated as writing nothing: its assignments are GONE, not empty, so advancing
    // onto one would claim surviving(child) == surviving(parent) on no evidence at all.
    if (o == nullptr || o->is_skeleton() || o->parents.size() != 1 || !(o->parents[0] == parent))
        return false;

    if (!cache.contains(parent))
        return false;

    auto const pinned = cache.is_pinned(parent);

    auto taken = cache.take(parent);
    CC_ASSERT(taken.has_value(), "the entry vanished between contains and take");
    auto doc = cc::move(taken.value());

    // An abstention withdraws the path instead of writing it, and must leave the snapshot byte-identical to a fresh
    // materialization — which is why clear_writers prunes the component and entity it empties.
    for (auto const a : o->assignments())
    {
        if (a.is_abstain())
            doc.clear_writers(a.path);
        else
            doc.set_single_writer(a.path, child, a.value.bytes());
    }

    // Overwriting leaves the old value bytes stranded in a chunk, so a long run of advances grows the snapshot without
    // bound unless it is periodically rebuilt.
    // Once the dead bytes outweigh the live ones the rebuild costs less than carrying them, and it is the only
    // O(document) event on this path — O(log) times per session rather than once per op.
    if (doc.dead_byte_size() * 2 > doc.owned_byte_size())
        doc = snapshot_document::create_owning_copy(doc.document());

    cache.install(child, cc::move(doc), pinned);
    return true;
}
