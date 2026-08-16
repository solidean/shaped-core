#include "op_graph.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/small_vector.hh>
#include <versioned-document/snapshot_cache.hh>

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

/// What one path knows at one point in the sweep.
///
/// `surviving` is the maximal writers so far; `superseded` is every writer a descendant has overwritten.
/// The second set is what makes dominance resolvable without any global ancestor query: each parent's ancestor set is
/// ancestor-closed, so a dominating pair always lands wholly inside one branch and can be recorded there.
struct path_state
{
    cc::small_vector<property_value, 1> surviving;
    cc::small_vector<op_id, 1> superseded;
};

/// One op's view of every path, dense by path index.
using materialize_state = cc::vector<path_state>;

[[nodiscard]] bool contains_writer(cc::span<property_value const> writers, op_id const& id)
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
void merge_into(materialize_state& dst, materialize_state const& src)
{
    for (isize i = 0; i < dst.size(); ++i)
    {
        auto& d = dst[i];
        auto const& s = src[i];
        if (s.surviving.empty() && s.superseded.empty())
            continue;

        for (auto const& id : s.superseded)
            if (!contains_id(d.superseded, id))
                d.superseded.push_back(id);

        for (auto const& w : s.surviving)
            if (!contains_writer(d.surviving, w.writer))
                d.surviving.push_back(w);

        // a writer one branch has already overwritten does not come back because another branch still lists it
        if (d.superseded.empty())
            continue;

        auto kept = cc::small_vector<property_value, 1>();
        for (auto const& w : d.surviving)
            if (!contains_id(d.superseded, w.writer))
                kept.push_back(w);

        d.surviving = cc::move(kept);
    }
}

/// Applies one write: everything currently surviving is now superseded, and this writer stands alone.
void apply_write(path_state& state, op_id const& writer, vdoc::value_view value)
{
    for (auto const& w : state.surviving)
        if (!contains_id(state.superseded, w.writer))
            state.superseded.push_back(w.writer);

    state.surviving.clear();
    state.surviving.push_back(property_value{.writer = writer, .value = value});
}

/// Assembles the sorted raw_document from the final per-path state.
[[nodiscard]] raw_document build_document(cc::span<property_path const> paths, materialize_state const& state)
{
    // Output is built by sorting an explicit vector, never by iterating a hash container, so the result is the same
    // on every machine and under every insertion order.
    struct flat_entry
    {
        property_path path;
        vdoc::raw_property property;
    };

    auto flat = cc::vector<flat_entry>();
    for (isize i = 0; i < paths.size(); ++i)
    {
        if (state[i].surviving.empty())
            continue;

        auto writers = cc::vector<property_value>();
        for (auto const& w : state[i].surviving)
            writers.push_back(w);

        std::sort(writers.begin(), writers.end(), [](property_value const& a, property_value const& b)
                  { return a.writer.compare_bytes(b.writer) < 0; });

        flat.push_back(flat_entry{.path = paths[i], .property = vdoc::raw_property{.writers = cc::move(writers)}});
    }

    std::sort(flat.begin(), flat.end(),
              [](flat_entry const& a, flat_entry const& b) { return a.path.compare_bytes(b.path) < 0; });

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

    std::sort(out.begin(), out.end(), op_id::by_bytes{});
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

    std::sort(out.begin(), out.end(), op_id::by_bytes{});
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
    for (auto const& e : entities)
        wanted.insert(e);
    auto const filtered = !entities.empty();

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
                has_unwalked_parent[i] = graph.contains(parent);
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
            for (auto const& e : snapshot.entities)
                for (auto const& c : e.value.components)
                    for (auto const& p : c.value.properties)
                        register_path({.entity = e.entity, .component = c.component, .property = p.property});

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
    auto states = cc::vector<cc::vector<path_state>>();
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
        auto state = cc::vector<path_state>();
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
                states[p] = {};
                has_state[p] = false;
                --live_states;
                stolen = true;
            }
        }

        if (!stolen)
            state.resize_to_defaulted(path_count);

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
            for (auto const& e : snapshot.entities)
                for (auto const& c : e.value.components)
                    for (auto const& p : c.value.properties)
                    {
                        auto const path
                            = property_path{.entity = e.entity, .component = c.component, .property = p.property};
                        if (filtered && !wanted.contains(path.entity))
                            continue;

                        auto& s = state[path_index[path]];
                        for (auto const& w : p.value.writers)
                            s.surviving.push_back(w);
                    }

            if (options.stats != nullptr)
                options.stats->snapshots_used = 1;
        }
        else
        {
            for (auto const a : o->assignments())
            {
                if (filtered && !wanted.contains(a.path.entity))
                    continue;

                apply_write(state[path_index[a.path]], o->id, a.value);
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
                states[p] = {};
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
        // it held when a snapshot was taken — see ../../docs/todo/milestone-6.md.
        if (options.drop_superseded_at_articulation_points && live_states == 1)
            for (auto& s : states[ix])
                s.superseded.clear();

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
    auto final_state = cc::vector<path_state>();
    final_state.resize_to_defaulted(path_count);
    for (auto const& head : heads)
    {
        auto const* const h = index_of.get_ptr(head);
        if (h == nullptr || !has_state[*h])
            continue;

        merge_into(final_state, states[*h]);
    }

    if (options.stats != nullptr)
        options.stats->ops_walked = processed;

    return build_document(paths, final_state);
}
