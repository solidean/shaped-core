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
    while (_entries.size() - pinned_count() > _budget.max_unpinned_entries)
    {
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
