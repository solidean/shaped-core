#include "incremental_parse.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>

using namespace cc::primitive_defines;

namespace
{
using namespace vdoc;

/// The single-parent chain from `to` back to `from`, nearest-first, or empty where there is none within the bound.
///
/// `found` distinguishes "no chain" from "already there": `to == from` is a genuine empty chain and a fast path with
/// nothing to do.
/// `reason` is only meaningful when `found` is false.
struct chain_walk
{
    bool found = false;
    apply_fallback_reason reason = apply_fallback_reason::no_single_parent_chain;
    cc::vector<op const*> ops;
};

[[nodiscard]] chain_walk walk_chain(op_graph const& graph, op_id const& from, op_id const& to, isize max_ops)
{
    auto out = chain_walk();
    if (to == from)
    {
        out.found = graph.contains(from);
        return out;
    }

    auto at = to;
    for (isize step = 0; step < max_ops; ++step)
    {
        auto const* const o = graph.find(at);

        // A skeleton's assignments are gone rather than empty, so a chain through one is not a delta anyone can read.
        if (o == nullptr || o->is_skeleton() || o->parents.size() != 1)
            return {};

        out.ops.push_back(o);
        at = o->parents[0];

        if (at == from)
        {
            out.found = true;
            return out;
        }
    }

    // Every step was the right shape, so what ran out was the bound and not the history.
    return {.reason = apply_fallback_reason::chain_too_long};
}

/// Every path the chain assigned to.
///
/// A single-parent chain's own assignments are its complete delta, which is what makes this the whole dirty set rather
/// than an approximation of one.
[[nodiscard]] change_set change_set_of(cc::span<op const* const> chain)
{
    auto builder = change_set_builder(change_granularity::property);

    for (auto const* const o : chain)
        for (auto const a : o->assignments())
            builder.add(a.path);

    return cc::move(builder).build();
}

/// Whether the report already carries a document-scoped diagnostic for this type.
[[nodiscard]] bool already_reported_unsupported(parse_report const& report, component_type_id type)
{
    for (auto const& d : report.diagnostics)
        if (d.kind == diagnostic_kind::unsupported_component_type && d.path.component == type)
            return true;

    return false;
}

void record_entity(change_summary& out, entity_id entity, change_kind kind)
{
    out.entities.push_back({.entity = entity, .kind = kind});
}

void record_component(change_summary& out, entity_id entity, component_type_id component, change_kind kind)
{
    out.components.push_back({.entity = entity, .component = component, .kind = kind});
}

void sort_summary(change_summary& out)
{
    cc::sort(out.entities, [](change_summary::entity_change const& a, change_summary::entity_change const& b)
             { return a.entity.compare_bytes(b.entity) < 0; });

    cc::sort(out.components,
             [](change_summary::component_change const& a, change_summary::component_change const& b)
             {
                 if (auto const by_entity = a.entity.compare_bytes(b.entity); by_entity != 0)
                     return by_entity < 0;
                 return a.component.compare_bytes(b.component) < 0;
             });
}

/// The whole document at `to`, parsed from scratch, plus the only summary this path can honestly give.
///
/// **The slow path's summary is conservative: everything.**
/// It has no chain to read a delta off, and diffing the two documents would still not produce the fast path's answer
/// — the fast path reports what it re-decided, and a diff reports what happens to differ.
/// So a caller that sees `took_fast_path == false` should treat every projection it holds as stale, which is exactly
/// what this summary says.
[[nodiscard]] document full_reparse(document&& before,
                                    op_graph const& graph,
                                    op_id const& to,
                                    parse_policy const& policy,
                                    parse_report& report,
                                    change_summary& out_changes,
                                    snapshot_cache* cache)
{
    auto const raw = cache == nullptr ? graph.materialize(to) : graph.materialize(to, *cache);

    // A full parse re-decides the whole document, so nothing a previous one recorded may survive it.
    report.clear();
    auto after = parse(raw, policy, report);

    auto const record_all_components = [&](document const& d, entity_id e, change_kind kind)
    {
        for (auto const& t : d.component_types())
            if (d.has_component(t, e))
                record_component(out_changes, e, t, kind);
    };

    for (auto const& e : before.entities())
        if (!after.contains(e))
        {
            record_all_components(before, e, change_kind::removed);
            record_entity(out_changes, e, change_kind::removed);
        }

    for (auto const& e : after.entities())
    {
        record_all_components(after, e, before.contains(e) ? change_kind::modified : change_kind::added);
        record_entity(out_changes, e, before.contains(e) ? change_kind::modified : change_kind::added);
    }

    sort_summary(out_changes);
    return after;
}
} // namespace

vdoc::document vdoc::apply(document&& doc,
                           op_graph const& graph,
                           op_id const& from,
                           op_id const& to,
                           parse_policy const& policy,
                           parse_report& report,
                           change_summary& out_changes,
                           incremental_apply_options options,
                           incremental_apply_stats* stats)
{
    out_changes.clear();
    if (stats != nullptr)
        *stats = {};

    auto const chain = options.force_full_reparse ? chain_walk{.reason = apply_fallback_reason::forced}
                                                  : walk_chain(graph, from, to, options.max_chain_ops);
    if (!chain.found)
    {
        if (stats != nullptr)
            stats->fallback_reason = chain.reason;

        return full_reparse(cc::move(doc), graph, to, policy, report, out_changes, options.cache);
    }

    // Entity granularity is what re-interpretation works at: a parse selects and constructs one entity at a time, so
    // knowing which property changed under it buys nothing here.
    auto dirty = change_set_of(chain.ops);
    dirty.coarsen_to(change_granularity::entity);

    auto const touched = dirty.entities();
    if (stats != nullptr)
    {
        stats->took_fast_path = true;
        stats->chain_ops = chain.ops.size();
        stats->touched_entities = touched.size();
    }

    if (touched.empty())
        return cc::move(doc);

    auto const raw = options.cache == nullptr
                       ? graph.materialize_entities(cc::span<op_id const>(&to, 1), touched)
                       : graph.materialize_entities(cc::span<op_id const>(&to, 1), touched, *options.cache);

    // Everything this apply is about to re-decide loses what a previous decision recorded, before anything re-decides
    // it — or the report would hold both answers.
    report.drop_for_entities(touched);

    auto builder = document_builder(cc::move(doc));
    auto unsupported = cc::vector<component_type_id>();

    for (auto const& entity : touched)
    {
        auto const* const raw_entity_at = raw.try_get(entity);
        auto const selection = raw_entity_at == nullptr
                                 ? impl::entity_selection()
                                 : impl::select_entity(entity, *raw_entity_at, policy, report, unsupported);

        auto const was_present = builder.contains_entity(entity);

        if (!selection.instantiate)
        {
            if (was_present)
            {
                // Report the components going with it before they go, so a caller can invalidate what it kept.
                for (auto const& t : builder.component_types())
                    if (builder.has_component(t, entity))
                        record_component(out_changes, entity, t, change_kind::removed);

                (void)builder.remove_entity(entity);
                record_entity(out_changes, entity, change_kind::removed);
            }
            continue;
        }

        if (!was_present)
        {
            (void)builder.insert_entity(entity);
            record_entity(out_changes, entity, change_kind::added);
        }
        else
            record_entity(out_changes, entity, change_kind::modified);

        // Whatever it used to carry and the selection no longer names is gone.
        for (auto const& t : builder.component_types())
        {
            auto wanted = false;
            for (auto const& sc : selection.components)
                wanted = wanted || sc.type == t;

            if (!wanted && builder.has_component(t, entity))
            {
                (void)builder.remove_component(t, entity);
                record_component(out_changes, entity, t, change_kind::removed);
            }
        }

        for (auto const& sc : selection.components)
        {
            auto const reader = property_reader::create_for(*sc.raw, policy, report, entity, sc.type, sc.version);
            auto const kind = builder.set_component(*sc.schema, entity,
                                                    [&](byte* slot) { return sc.schema->parse_into(slot, reader); });

            record_component(out_changes, entity, sc.type, kind);
        }
    }

    // Document-scoped and never retracted: knowing a type is GONE needs a walk of the whole document, which is the one
    // thing this path exists to avoid.
    cc::sort(unsupported, component_type_id::by_bytes{});
    for (auto const& t : unsupported)
        if (!already_reported_unsupported(report, t))
            report.diagnostics.push_back({.kind = diagnostic_kind::unsupported_component_type, .path = {.component = t}});

    if (options.compaction_ratio > 0
        && f64(builder.dead_arena_bytes()) > options.compaction_ratio * f64(builder.live_arena_bytes()))
    {
        builder.compact();
        if (stats != nullptr)
            stats->compacted = true;
    }

    sort_summary(out_changes);
    return cc::move(builder).freeze();
}
