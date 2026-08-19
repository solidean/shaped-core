#include "op_graph.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/small_vector.hh>
#include <versioned-document/snapshot_cache.hh>

// only for the binary heap the Kahn sweep pops in id order; every sort here is cc::sort
#include <algorithm>

using namespace cc::primitive_defines;

namespace
{
using vdoc::entity_id;
using vdoc::op;
using vdoc::op_id;
using vdoc::property_path;
using vdoc::property_value;
using vdoc::raw_document;
using vdoc::raw_property;

/// Walks the snapshot properties a sweep actually needs, and reports how many of its entities were read.
///
/// A FILTERED sweep looks its entities up rather than walking the whole snapshot and discarding almost all of it.
/// The snapshot is a whole document, so walking it is what a snapshot costs; looking up is what makes seeding worth
/// anything at the one-entity diff `op_builder::build` asks for.
template <class F>
[[nodiscard]] isize for_each_wanted_property(raw_document const& snapshot,
                                             bool filtered,
                                             cc::span<entity_id const> wanted_sorted,
                                             F&& fn)
{
    auto const visit = [&](entity_id entity, vdoc::raw_entity const& e)
    {
        for (auto const& c : e.components)
            for (auto const& p : c.value.properties)
                fn(property_path{.entity = entity, .component = c.component, .property = p.property}, p.value);
    };

    if (!filtered)
    {
        for (auto const& e : snapshot.entities)
            visit(e.entity, e.value);

        return snapshot.entities.size();
    }

    // wanted_sorted is in entity byte order and so is the snapshot, but the two are unrelated in size — a binary
    // search each is the right shape rather than a merge.
    auto read = isize(0);
    for (auto const& want : wanted_sorted)
        if (auto const* const e = snapshot.try_get(want))
        {
            visit(want, *e);
            ++read;
        }

    return read;
}

/// A surviving writer as the sweep sees one, which unlike a raw document's may be a withdrawal rather than a value.
///
/// **This is the only place an abstention exists.**
/// It supersedes its ancestors exactly as a write does and then contributes nothing, so `build_document` drops it and a
/// `raw_document` never carries one — which is what keeps abstain out of the snapshot format and out of every consumer
/// that reads a writer list.
struct sweep_writer
{
    op_id writer;
    vdoc::value_view value;

    /// When set, `value` is meaningless: this writer withdrew rather than wrote.
    bool abstains = false;
};

/// What one path knows at one point in the sweep.
///
/// `surviving` is the maximal writers so far; `superseded` is every writer a descendant has overwritten.
/// The second set is what makes dominance resolvable without any global ancestor query: each parent's ancestor set is
/// ancestor-closed, so a dominating pair always lands wholly inside one branch and can be recorded there.
struct path_state
{
    cc::small_vector<sweep_writer, 1> surviving;
    cc::small_vector<op_id, 1> superseded;

    /// Membership flags for the side lists below, so appending to one is O(1) and needs no set.
    bool in_occupied = false;
    bool in_dirty = false;
};

/// One op's view of every path, dense by path index, plus the indices that are actually live.
///
/// The slots are dense because a path index is what everything else is keyed by, but almost all of them are empty on
/// any real document: an op touches a handful of paths and the document has tens of thousands.
/// So every pass that would otherwise walk all of them walks a side list instead — and the lists travel with a stolen
/// state, which is what keeps a linear history free of per-op copies.
///
/// Both lists are SUPERSETS: an index goes in when its slot first becomes non-empty and never comes out, because a
/// slot can be emptied again by a merge or by the articulation clear.
/// Every consumer re-checks the slot, so a stale index costs one comparison and never a wrong answer.
struct sweep_state
{
    cc::vector<path_state> slots;

    /// Indices whose surviving or superseded has been non-empty — what merge_into and build_document iterate.
    cc::vector<i32> occupied;

    /// Indices whose superseded has been non-empty — what the articulation clear walks.
    cc::vector<i32> dirty;
};

void note_occupied(sweep_state& state, i32 path)
{
    if (state.slots[path].in_occupied)
        return;

    state.slots[path].in_occupied = true;
    state.occupied.push_back(path);
}

void note_dirty(sweep_state& state, i32 path)
{
    if (state.slots[path].in_dirty)
        return;

    state.slots[path].in_dirty = true;
    state.dirty.push_back(path);
}

[[nodiscard]] bool contains_writer(cc::span<sweep_writer const> writers, op_id const& id)
{
    for (auto const& w : writers)
        if (w.writer == id)
            return true;
    return false;
}

[[nodiscard]] bool contains_id(cc::span<op_id const> ids, op_id const& id)
{
    for (auto const& v : ids)
        if (v == id)
            return true;
    return false;
}

/// Merges `src` into `dst`: union both sets, then drop anything the combined superseded covers.
void merge_into(sweep_state& dst, sweep_state const& src)
{
    for (auto const i : src.occupied)
    {
        auto const& s = src.slots[i];
        if (s.surviving.empty() && s.superseded.empty())
            continue;

        auto& d = dst.slots[i];

        for (auto const& id : s.superseded)
            if (!contains_id(d.superseded, id))
                d.superseded.push_back(id);

        for (auto const& w : s.surviving)
            if (!contains_writer(d.surviving, w.writer))
                d.surviving.push_back(w);

        note_occupied(dst, i);
        if (!d.superseded.empty())
            note_dirty(dst, i);

        // a writer one branch has already overwritten does not come back because another branch still lists it
        if (d.superseded.empty())
            continue;

        auto kept = cc::small_vector<sweep_writer, 1>();
        for (auto const& w : d.surviving)
            if (!contains_id(d.superseded, w.writer))
                kept.push_back(w);

        d.surviving = cc::move(kept);
    }
}

/// Applies one assignment: everything currently surviving is now superseded, and this writer stands alone.
///
/// An abstention takes exactly this path — superseding is the whole of what it does, and `value` is then meaningless.
void apply_write(sweep_state& state, i32 path, op_id const& writer, vdoc::value_view value, bool abstains)
{
    auto& slot = state.slots[path];

    for (auto const& w : slot.surviving)
        if (!contains_id(slot.superseded, w.writer))
            slot.superseded.push_back(w.writer);

    slot.surviving.clear();
    slot.surviving.push_back(sweep_writer{.writer = writer, .value = value, .abstains = abstains});

    note_occupied(state, path);
    if (!slot.superseded.empty())
        note_dirty(state, path);
}

/// Whether the side lists still cover every non-empty slot, with no index listed twice.
///
/// The one thing that can go wrong with a side list is a missed record, and a missed record is invisible: the sweep
/// simply skips work it owed and returns a plausible wrong answer.
/// So it is checked directly, under an assert, rather than inferred from the results agreeing.
[[nodiscard, maybe_unused]] bool side_lists_cover(sweep_state const& state)
{
    auto occupied_flags = isize(0);
    auto dirty_flags = isize(0);

    for (auto const& s : state.slots)
    {
        if ((!s.surviving.empty() || !s.superseded.empty()) && !s.in_occupied)
            return false;
        if (!s.superseded.empty() && !s.in_dirty)
            return false;

        occupied_flags += s.in_occupied ? 1 : 0;
        dirty_flags += s.in_dirty ? 1 : 0;
    }

    // A duplicate push would leave more entries than flagged slots, which no amount of re-checking downstream catches.
    return occupied_flags == state.occupied.size() && dirty_flags == state.dirty.size();
}

/// Assembles the sorted raw_document from the final per-path state.
[[nodiscard]] raw_document build_document(cc::span<property_path const> paths, sweep_state const& state)
{
    // Output is built by sorting an explicit vector, never by iterating a hash container, so the result is the same
    // on every machine and under every insertion order.
    struct flat_entry
    {
        property_path path;
        vdoc::raw_property property;
    };

    auto flat = cc::vector<flat_entry>();
    for (auto const i : state.occupied)
    {
        if (state.slots[i].surviving.empty())
            continue;

        // An abstaining writer is dropped here, which is the whole of how abstain stays out of the raw document.
        // A path whose every survivor abstained therefore ends up absent — indistinguishable from never written, which
        // is exactly what a withdrawal means.
        auto writers = cc::vector<property_value>();
        for (auto const& w : state.slots[i].surviving)
            if (!w.abstains)
                writers.push_back(property_value{.writer = w.writer, .value = w.value});

        if (writers.empty())
            continue;

        cc::sort(writers,
                 [](property_value const& a, property_value const& b) { return a.writer.compare_bytes(b.writer) < 0; });

        flat.push_back(flat_entry{.path = paths[i], .property = vdoc::raw_property{.writers = cc::move(writers)}});
    }

    cc::sort(flat, [](flat_entry const& a, flat_entry const& b) { return a.path.compare_bytes(b.path) < 0; });

    auto out = raw_document();
    for (auto& e : flat)
    {
        if (out.entities.empty() || out.entities.back().entity != e.path.entity)
            out.entities.push_back(raw_document::entry{.entity = e.path.entity, .value = {}});

        auto& entity = out.entities.back().value;
        if (entity.components.empty() || entity.components.back().component != e.path.component)
            entity.components.push_back(vdoc::raw_entity::entry{.component = e.path.component, .value = {}});

        auto& component = entity.components.back().value;
        component.properties.push_back(
            vdoc::raw_component::entry{.property = e.path.property, .value = cc::move(e.property)});
    }

    return out;
}
} // namespace

vdoc::op_id vdoc::op_graph::add(op o)
{
    auto const id = o.id;

    // Idempotent: the same content is the same op, so re-adding it must leave the graph exactly as it was.
    // In particular the child index is not touched, or a duplicate add would list every child twice.
    if (_ops.contains(id))
        return id;

    for (auto const& parent : o.parents)
    {
        auto& siblings = _children[parent];
        if (!contains_id(siblings, id))
            siblings.push_back(id);
    }

    _ops[id] = cc::move(o);
    return id;
}

vdoc::op const* vdoc::op_graph::find(op_id const& id) const
{
    return _ops.get_ptr(id);
}

cc::span<vdoc::op_id const> vdoc::op_graph::children(op_id const& id) const
{
    auto const* const siblings = _children.get_ptr(id);
    return siblings ? cc::span<op_id const>(*siblings) : cc::span<op_id const>();
}

cc::vector<vdoc::op_id> vdoc::op_graph::collect_reachable(cc::span<op_id const> heads) const
{
    auto seen = cc::set<op_id>();
    auto stack = cc::vector<op_id>();
    auto out = cc::vector<op_id>();

    for (auto const& head : heads)
        stack.push_back(head);

    while (!stack.empty())
    {
        auto const id = stack.back();
        stack.remove_back();

        if (seen.contains(id))
            continue;

        // A missing op is skipped rather than reported: a pruned parent, or one a peer has not sent yet, is a normal
        // state of the graph and not a failure of the walk.
        auto const* const o = find(id);
        if (o == nullptr)
            continue;

        seen.insert(id);
        out.push_back(id);

        for (auto const& parent : o->parents)
            stack.push_back(parent);
    }

    cc::sort(out, op_id::by_bytes{});
    return out;
}

cc::vector<vdoc::op_id> vdoc::op_graph::collect_reachable_until(cc::span<op_id const> heads,
                                                                cc::function_ref<bool(op_id const&)> is_terminator) const
{
    auto seen = cc::set<op_id>();
    auto stack = cc::vector<op_id>();
    auto out = cc::vector<op_id>();

    for (auto const& head : heads)
        stack.push_back(head);

    while (!stack.empty())
    {
        auto const id = stack.back();
        stack.remove_back();

        if (seen.contains(id))
            continue;

        auto const* const o = find(id);
        if (o == nullptr)
            continue;

        seen.insert(id);
        out.push_back(id);

        // A terminator is in the result but its parents are not expanded, which is what leaves the result
        // parent-closed EXCEPT at terminators — the property the validity gate reads off the in-degrees.
        // A head that is itself a terminator terminates too; nothing here special-cases one.
        if (is_terminator(id))
            continue;

        for (auto const& parent : o->parents)
            stack.push_back(parent);
    }

    cc::sort(out, op_id::by_bytes{});
    return out;
}

bool vdoc::op_graph::skeletonize(op_id const& id)
{
    auto* const o = _ops.get_ptr(id);
    if (o == nullptr)
        return false;

    // The child index is untouched on purpose: a skeleton keeps every edge, and that is the whole point of it.
    o->payload = {};
    return true;
}

bool vdoc::op_graph::fill_payload(op_id const& id, op_payload payload)
{
    auto* const o = _ops.get_ptr(id);
    if (o == nullptr)
        return false;

    // Content addressing makes a second payload for the same id the same bytes, so a full op is left as it is rather
    // than rewritten — and the edges never move, because filling changes no parent.
    if (o->is_skeleton())
        o->payload = cc::move(payload);

    return true;
}

bool vdoc::op_graph::drop_leaf(op_id const& id)
{
    auto const* const o = _ops.get_ptr(id);
    if (o == nullptr)
        return false;

    CC_ASSERT(children(id).empty(), "drop_leaf on an op something descends from - keeping ancestry is skeletonize's "
                                    "job");

    // Read out before the entry goes, since erasing invalidates `o`.
    auto const parents = o->parents;
    _ops.erase(id);
    _children.erase(id);

    for (auto const& parent : parents)
    {
        auto* const siblings = _children.get_ptr(parent);
        if (siblings == nullptr)
            continue;

        // Shifted rather than swapped with the back, because children() reports arrival order and a drop must not
        // reshuffle the frames that are still there.
        auto at = isize(0);
        while (at < siblings->size() && !((*siblings)[at] == id))
            ++at;

        for (auto i = at; i + 1 < siblings->size(); ++i)
            (*siblings)[i] = (*siblings)[i + 1];

        if (at < siblings->size())
            siblings->remove_back();

        if (siblings->empty())
            _children.erase(parent);
    }

    return true;
}

cc::vector<vdoc::op_id> vdoc::op_graph::leaves() const
{
    auto out = cc::vector<op_id>();
    for (auto const& [id, o] : _ops)
        if (children(id).empty())
            out.push_back(id);

    // _ops is a hash map, so the order it yields is not one anything may depend on.
    cc::sort(out, op_id::by_bytes{});
    return out;
}

vdoc::raw_document vdoc::op_graph::materialize(op_id const& head) const
{
    op_id const heads[] = {head};
    return impl::materialize(*this, heads, {}, {});
}

vdoc::raw_document vdoc::op_graph::materialize(cc::span<op_id const> heads) const
{
    return impl::materialize(*this, heads, {}, {});
}

vdoc::raw_document vdoc::op_graph::materialize_entities(cc::span<op_id const> heads,
                                                        cc::span<entity_id const> entities) const
{
    return impl::materialize(*this, heads, entities, {});
}

vdoc::raw_document vdoc::op_graph::materialize(op_id const& head, snapshot_cache& cache) const
{
    op_id const heads[] = {head};
    return impl::materialize(*this, heads, {}, {.cache = &cache});
}

vdoc::raw_document vdoc::op_graph::materialize(cc::span<op_id const> heads, snapshot_cache& cache) const
{
    return impl::materialize(*this, heads, {}, {.cache = &cache});
}

vdoc::raw_document vdoc::op_graph::materialize_entities(cc::span<op_id const> heads,
                                                        cc::span<entity_id const> entities,
                                                        snapshot_cache& cache) const
{
    return impl::materialize(*this, heads, entities, {.cache = &cache});
}

vdoc::raw_document vdoc::impl::materialize(op_graph const& graph,
                                           cc::span<op_id const> heads,
                                           cc::span<entity_id const> entities,
                                           materialize_options options)
{
    // The walk stops at cached snapshots where there are any, which is the whole of what makes a long history cost
    // what a short one does.
    // Whether stopping there was SOUND is decided below, against this DAG as it is today.
    auto const reachable
        = options.cache == nullptr
            ? graph.collect_reachable(heads)
            : graph.collect_reachable_until(heads, [&](op_id const& id) { return options.cache->contains(id); });
    auto const n = reachable.size();

    auto index_of = cc::map<op_id, i32>();
    for (isize i = 0; i < n; ++i)
        index_of[reachable[i]] = i32(i);

    // The filter is a predicate on ASSIGNMENTS, never on edges.
    // Filtering edges would sever ancestry and fabricate multi-values, exactly as deleting an op outright would.
    auto wanted = cc::set<entity_id>();
    auto wanted_sorted = cc::vector<entity_id>();
    for (auto const& e : entities)
        if (wanted.insert(e))
            wanted_sorted.push_back(e);
    auto const filtered = !entities.empty();

    // A snapshot is reached through this list rather than through the set, so nothing about a hash container's order
    // can decide which path index is allocated first.
    // build_document sorts at the end either way; this simply removes the question.
    cc::sort(wanted_sorted, entity_id::by_bytes{});

    // ---- pass 1a: the forward edges the sweep walks --------------------------------------------------------------
    auto forward = cc::vector<cc::vector<i32>>();
    forward.resize_to_defaulted(n);
    auto in_degree = cc::vector<i32>();
    in_degree.resize_to_filled(n, 0);
    auto remaining_consumers = cc::vector<i32>();
    remaining_consumers.resize_to_filled(n, 0);

    // An op whose parent is in the graph but outside the walk — which only a terminated walk can produce, and which
    // means this op's state would be built from an incomplete set of ancestors.
    auto has_unwalked_parent = cc::vector<bool>();
    has_unwalked_parent.resize_to_filled(n, false);

    for (isize i = 0; i < n; ++i)
    {
        auto const* const o = graph.find(reachable[i]);
        for (auto const& parent : o->parents)
        {
            auto const* const p = index_of.get_ptr(parent);
            if (p == nullptr)
            {
                // An op the graph does not have contributes no edge, which is what makes a pruned history walkable.
                // One the graph DOES have, but the walk stopped short of, is a different thing entirely.
                //
                // Accumulated rather than assigned: a merge op with one such parent and one genuinely-absent parent
                // would otherwise have the flag cleared by whichever came last.
                // The gate below cannot currently reach that case — a parent inside the graph but outside the walk
                // only arises at a terminator, and a terminator has no walked parent, so it is always a source — but
                // the next terminator kind added would reach it.
                has_unwalked_parent[i] = has_unwalked_parent[i] || graph.contains(parent);
                continue;
            }

            forward[*p].push_back(i32(i));
            ++in_degree[i];
            ++remaining_consumers[*p];
        }
    }

    // ---- the validity gate: may this sweep be seeded from a snapshot at all? -------------------------------------
    //
    // A snapshot holds `surviving` and no `superseded`, so seeding one is sound exactly where nothing else in this
    // sweep can present a writer that the missing `superseded` would have suppressed.
    //
    // That holds when the walked set has EXACTLY ONE source and it is the snapshot: every non-source has a parent
    // inside the walk, so descending parent edges from any op lands on that single source, making it an ancestor of
    // everything walked — and an ancestor cannot present a stale branch.
    //
    // "Every source is cached" would NOT be enough.
    // Materializing {T, X} where X is a distant ancestor of T gives two cached sources, and unions their surviving
    // sets into a multi-value nobody wrote.
    // The minimal condition is pairwise-incomparable sources, but comparing them is the global ancestor query
    // ../../docs/decisions.md declines to pay for, and exactly-one is the cheap sufficient case that fits real
    // histories.
    auto seeded = i32(-1);
    if (options.cache != nullptr)
    {
        auto any_terminator = false;
        for (isize i = 0; i < n && !any_terminator; ++i)
            any_terminator = options.cache->contains(reachable[i]);

        if (any_terminator)
        {
            auto sources = cc::vector<i32>();
            for (isize i = 0; i < n; ++i)
                if (in_degree[i] == 0)
                    sources.push_back(i32(i));

            auto accepted = sources.size() == 1 && options.cache->contains(reachable[sources[0]]);

            // Everything the walk stopped short of must be behind that one source, or some op is being replayed
            // from a partial set of ancestors.
            for (isize i = 0; accepted && i < n; ++i)
                accepted = !has_unwalked_parent[i] || i32(i) == sources[0];

            if (!accepted)
            {
                // Self-correcting rather than optimistic: the sweep simply replays, which costs time and never a
                // result.
                // This is the case a branch reaching around a snapshot lands in.
                if (options.stats != nullptr)
                    options.stats->fell_back = true;

                auto plain = options;
                plain.cache = nullptr;
                return materialize(graph, heads, entities, plain);
            }

            seeded = sources[0];
        }
    }

    // ---- pass 1b: dense path indices ----------------------------------------------------------------------------
    //
    // This map is probed and never iterated, so its order cannot reach the output.
    auto path_index = cc::map<property_path, i32>();
    auto paths = cc::vector<property_path>();

    auto const register_path = [&](property_path const& path)
    {
        if (filtered && !wanted.contains(path.entity))
            return;

        auto entry = path_index.entry(path);
        if (!entry.exists())
        {
            auto const next = i32(paths.size());
            paths.push_back(path);
            entry.emplace(next);
        }
    };

    for (isize i = 0; i < n; ++i)
    {
        // The seeded op's own writes are already in its snapshot, so its assignments are not registered here and are
        // not applied later either.
        if (i32(i) == seeded)
        {
            auto const& snapshot = options.cache->find(reachable[i])->document();
            (void)for_each_wanted_property(snapshot, filtered, wanted_sorted,
                                           [&](property_path const& path, raw_property const&) { register_path(path); });

            continue;
        }

        auto const* const o = graph.find(reachable[i]);
        for (auto const a : o->assignments())
            register_path(a.path);
    }

    // a head's state is read at the very end, so it is a consumer like any child
    for (auto const& head : heads)
        if (auto const* const h = index_of.get_ptr(head))
            ++remaining_consumers[*h];

    auto const path_count = paths.size();

    // ---- pass 2: propagate state in topological order ------------------------------------------------------------
    auto states = cc::vector<sweep_state>();
    states.resize_to_defaulted(n);
    auto has_state = cc::vector<bool>();
    has_state.resize_to_filled(n, false);
    auto live_states = isize(0);

    // Kahn, with the ready set popped in id-byte order.
    // `reachable` is already sorted that way, so the smallest index is the smallest id, which keeps the topological
    // order itself reproducible.
    auto ready = cc::vector<i32>();
    for (isize i = 0; i < n; ++i)
        if (in_degree[i] == 0)
            ready.push_back(i32(i));
    std::make_heap(ready.begin(), ready.end(), std::greater<i32>());

    auto processed = isize(0);
    while (!ready.empty())
    {
        std::pop_heap(ready.begin(), ready.end(), std::greater<i32>());
        auto const ix = ready.back();
        ready.remove_back();
        ++processed;

        auto const* const o = graph.find(reachable[ix]);

        // Steal the state of a parent we are the last consumer of, and merge the rest into it.
        // Total copies are then one per excess edge rather than one per op, so a linear history copies nothing.
        auto state = sweep_state();
        auto stolen = false;
        auto parent_indices = cc::vector<i32>();
        for (auto const& parent : o->parents)
            if (auto const* const p = index_of.get_ptr(parent))
                parent_indices.push_back(*p);

        for (auto const p : parent_indices)
        {
            if (!has_state[p])
                continue;

            if (!stolen && remaining_consumers[p] == 1)
            {
                state = cc::move(states[p]);
                states[p] = sweep_state();
                has_state[p] = false;
                --live_states;
                stolen = true;
            }
        }

        if (!stolen)
            state.slots.resize_to_defaulted(path_count);

        for (auto const p : parent_indices)
            if (has_state[p])
                merge_into(state, states[p]);

        if (ix == seeded)
        {
            // The snapshot IS this op's state: surviving as stored, and nothing superseded, which the gate above has
            // just established that no op in this sweep can exploit.
            //
            // Its own assignments must NOT be re-applied.
            // surviving(T) already contains T's writes, so applying them again would move T into superseded while it
            // is also in surviving, and the next merge would drop it — T's own writes would silently vanish.
            auto const& snapshot = options.cache->find(reachable[ix])->document();
            auto const entities_read = for_each_wanted_property(
                snapshot, filtered, wanted_sorted,
                [&](property_path const& path, raw_property const& property)
                {
                    auto const at = path_index[path];

                    // A snapshot is a raw document, so nothing in it abstains.
                    for (auto const& w : property.writers)
                        state.slots[at].surviving.push_back(sweep_writer{.writer = w.writer, .value = w.value});

                    note_occupied(state, at);
                });

            if (options.stats != nullptr)
            {
                options.stats->snapshots_used = 1;
                options.stats->snapshot_entities_read = entities_read;
            }
        }
        else
        {
            for (auto const a : o->assignments())
            {
                if (filtered && !wanted.contains(a.path.entity))
                    continue;

                apply_write(state, path_index[a.path], o->id, a.value, a.is_abstain());
            }
        }

        states[ix] = cc::move(state);
        has_state[ix] = true;
        ++live_states;

        // Release parents whose last consumer we were.
        for (auto const p : parent_indices)
        {
            --remaining_consumers[p];
            if (remaining_consumers[p] == 0 && has_state[p])
            {
                states[p] = sweep_state();
                has_state[p] = false;
                --live_states;
            }
        }

        // At an articulation point every op still to be processed descends from this one, so nothing that arrives
        // later can present a branch carrying a writer these sets would have suppressed.
        //
        // The justification is per-SWEEP and does not survive being stored: a snapshot outlives its sweep, and a
        // user who later branches from before it reintroduces exactly what was dropped here.
        // Which is why the gate above re-establishes the same property against today's DAG rather than trusting that
        // it held when a snapshot was taken — see ../../docs/concepts/snapshots.md.
        //
        // Walking `dirty` rather than every slot is what makes this affordable at all: in a LINEAR history every op is
        // an articulation point, so a dense clear would run once per op over the whole path set.
        if (options.drop_superseded_at_articulation_points && live_states == 1)
        {
            auto& live = states[ix];
            for (auto const i : live.dirty)
            {
                live.slots[i].superseded.clear();
                live.slots[i].in_dirty = false;
            }
            live.dirty.clear();
        }

        for (auto const child : forward[ix])
        {
            --in_degree[child];
            if (in_degree[child] == 0)
            {
                ready.push_back(child);
                std::push_heap(ready.begin(), ready.end(), std::greater<i32>());
            }
        }
    }

    // A content-addressed DAG cannot contain a cycle, since an op id commits to everything behind it.
    // A hand-built or hostile op set can, and then Kahn simply stops early: the sweep terminates on what it could
    // order rather than spinning, and the ops in the cycle contribute nothing.
    CC_ASSERT(processed <= n, "the topological sweep visited an op twice");

    // ---- the result is the merge of the heads' states ------------------------------------------------------------
    auto final_state = sweep_state();
    final_state.slots.resize_to_defaulted(path_count);
    for (auto const& head : heads)
    {
        auto const* const h = index_of.get_ptr(head);
        if (h == nullptr || !has_state[*h])
            continue;

        CC_ASSERT(side_lists_cover(states[*h]), "the sweep lost track of a live path");
        merge_into(final_state, states[*h]);
    }
    CC_ASSERT(side_lists_cover(final_state), "the head merge lost track of a live path");

    if (options.stats != nullptr)
        options.stats->ops_walked = processed;

    return build_document(paths, final_state);
}
