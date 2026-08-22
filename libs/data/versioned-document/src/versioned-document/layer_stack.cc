#include "layer_stack.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/profiling.hh>

using namespace cc::primitive_defines;

namespace
{
using namespace vdoc;

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

/// Whether the report already carries a document-scoped diagnostic for this type.
[[nodiscard]] bool already_reported_unsupported(parse_report const& report, component_type_id type)
{
    for (auto const& d : report.diagnostics)
        if (d.kind == diagnostic_kind::unsupported_component_type && d.path.component == type)
            return true;

    return false;
}

void report_schema_version_conflicts(parse_report& report, impl::composed_document const& composed)
{
    for (auto const& path : composed.schema_version_conflicts)
        report.diagnostics.push_back({.kind = diagnostic_kind::layered_schema_version_conflict, .path = path});
}
} // namespace

vdoc::layer_stack::layer_stack() = default;
vdoc::layer_stack::~layer_stack() = default;
vdoc::layer_stack::layer_stack(layer_stack&&) noexcept = default;
vdoc::layer_stack& vdoc::layer_stack::operator=(layer_stack&&) noexcept = default;

vdoc::layer_handle vdoc::layer_stack::push_graph_layer(cc::string_view name,
                                                       op_graph const& graph,
                                                       op_id head,
                                                       snapshot_cache* cache)
{
    auto const generation = _next_generation++;
    _layers.push_back(
        layer{.name = cc::string(name), .generation = generation, .graph = &graph, .head = head, .cache = cache});
    _materialized.push_back(raw_document());

    _structure_dirty = true;
    return {.index = u32(_layers.size() - 1), .generation = generation};
}

vdoc::layer_handle vdoc::layer_stack::push_direct_layer(cc::string_view name, direct_layer& layer_ref)
{
    auto const generation = _next_generation++;
    _layers.push_back(layer{.name = cc::string(name), .generation = generation, .direct = &layer_ref});
    _materialized.push_back(raw_document());

    _structure_dirty = true;
    return {.index = u32(_layers.size() - 1), .generation = generation};
}

void vdoc::layer_stack::set_head(layer_handle handle, op_id head)
{
    auto* const l = impl_find(handle);
    CC_ASSERT(l != nullptr, "the handle names no layer in this stack");
    CC_ASSERT(l->is_graph(), "only a graph-backed layer has a head");

    l->head = head;
}

vdoc::op_id vdoc::layer_stack::head_of(layer_handle handle) const
{
    auto const* const l = impl_find(handle);
    CC_ASSERT(l != nullptr && l->is_graph(), "the handle names no graph-backed layer in this stack");
    return l->head;
}

void vdoc::layer_stack::set_muted(layer_handle handle, bool muted)
{
    auto* const l = impl_find(handle);
    CC_ASSERT(l != nullptr, "the handle names no layer in this stack");

    if (l->muted == muted)
        return;

    l->muted = muted;
    _structure_dirty = true;
}

bool vdoc::layer_stack::is_muted(layer_handle handle) const
{
    auto const* const l = impl_find(handle);
    CC_ASSERT(l != nullptr, "the handle names no layer in this stack");
    return l->muted;
}

vdoc::isize vdoc::layer_stack::layer_count() const
{
    return _layers.size();
}

vdoc::layer_handle vdoc::layer_stack::layer_at(isize index) const
{
    CC_ASSERT(index >= 0 && index < _layers.size(), "no layer at that index");
    return {.index = u32(index), .generation = _layers[index].generation};
}

vdoc::layer_stack::layer* vdoc::layer_stack::impl_find(layer_handle handle)
{
    if (!handle.is_valid() || isize(handle.index) >= _layers.size())
        return nullptr;

    auto& l = _layers[isize(handle.index)];
    return l.generation == handle.generation ? &l : nullptr;
}

vdoc::layer_stack::layer const* vdoc::layer_stack::impl_find(layer_handle handle) const
{
    if (!handle.is_valid() || isize(handle.index) >= _layers.size())
        return nullptr;

    auto const& l = _layers[isize(handle.index)];
    return l.generation == handle.generation ? &l : nullptr;
}

cc::vector<vdoc::impl::layer_view> vdoc::layer_stack::impl_views() const
{
    auto out = cc::vector<impl::layer_view>();
    for (auto i = isize(0); i < _layers.size(); ++i)
    {
        auto const& l = _layers[i];
        if (l.muted)
            continue;

        out.push_back({.document = l.is_graph() ? &_materialized[i] : &l.direct->document()});
    }

    return out;
}

void vdoc::layer_stack::impl_materialize(cc::span<entity_id const> entities)
{
    for (auto i = isize(0); i < _layers.size(); ++i)
    {
        auto const& l = _layers[i];
        if (l.muted || !l.is_graph())
        {
            _materialized[i] = raw_document();
            continue;
        }

        op_id const heads[] = {l.head};

        // A filtered materialization is a projection, which is exactly what composing a dirty subset wants — and the
        // reason it must never reach a snapshot cache.
        if (entities.empty())
            _materialized[i] = l.cache == nullptr ? l.graph->materialize(l.head) : l.graph->materialize(l.head, *l.cache);
        else
            _materialized[i] = l.cache == nullptr ? l.graph->materialize_entities(heads, entities)
                                                  : l.graph->materialize_entities(heads, entities, *l.cache);
    }
}

void vdoc::layer_stack::impl_record_versions()
{
    for (auto& l : _layers)
    {
        l.recorded_head = l.head;
        l.recorded_version = l.direct == nullptr ? 0 : l.direct->version();
    }
}

void vdoc::layer_stack::impl_discard_pending_changes()
{
    for (auto& l : _layers)
        if (l.direct != nullptr)
            (void)l.direct->impl_take_changes();
}

void vdoc::layer_stack::rebuild(parse_policy const& policy, parse_report& report, change_summary& out_changes)
{
    // The full-rebuild path, which is what an incremental apply falls back to — worth telling apart in a trace.
    CC_RECORD_SCOPE("vdoc.layer_stack.rebuild");

    out_changes.clear();

    impl_materialize({});
    auto const views = impl_views();
    auto const entities = impl::compose_entities(views);
    auto const composed = impl::compose(views, entities);

    // A full compose re-decides the whole document, so nothing a previous one recorded may survive it.
    report.clear();
    report_schema_version_conflicts(report, composed);
    auto after = impl::parse_from(entities, [&](entity_id e) { return composed.try_get(e); }, policy, report);

    // Conservative, exactly as an unlayered full re-parse is: there is no delta to read, so every projection a caller
    // holds must be treated as stale.
    auto const record_all_components = [&](document const& d, entity_id e, change_kind kind)
    {
        for (auto const& t : d.component_types())
            if (d.has_component(t, e))
                record_component(out_changes, e, t, kind);
    };

    for (auto const& e : _composed.entities())
        if (!after.contains(e))
        {
            record_all_components(_composed, e, change_kind::removed);
            record_entity(out_changes, e, change_kind::removed);
        }

    for (auto const& e : after.entities())
    {
        auto const kind = _composed.contains(e) ? change_kind::modified : change_kind::added;
        record_all_components(after, e, kind);
        record_entity(out_changes, e, kind);
    }

    sort_summary(out_changes);

    _composed = cc::move(after);
    impl_discard_pending_changes();
    impl_record_versions();
    _structure_dirty = false;
}

void vdoc::layer_stack::apply(parse_policy const& policy,
                              parse_report& report,
                              change_summary& out_changes,
                              layered_apply_options options,
                              layered_apply_stats* stats)
{
    CC_RECORD_SCOPE("vdoc.layer_stack.apply");

    out_changes.clear();
    if (stats != nullptr)
        *stats = {};

    auto const fall_back = [&](apply_fallback_reason reason, layer_handle blamed)
    {
        if (stats != nullptr)
        {
            stats->fallback_reason = reason;
            stats->stale_layer = blamed;
        }

        rebuild(policy, report, out_changes);
    };

    if (options.force_rebuild)
    {
        fall_back(apply_fallback_reason::forced, {});
        return;
    }

    // A layer coming, going or being muted changes the composition wholesale, and enumerating what it changed means
    // enumerating that layer's paths — which for a base layer is the whole document.
    if (_structure_dirty)
    {
        fall_back(apply_fallback_reason::structure_changed, {});
        return;
    }

    // Every delta is PULLED.
    // The stack holds each graph layer's head and each direct layer bumps its own version, so there is no way for a
    // caller to forget to mention that one moved.
    auto dirty = change_set();
    for (auto i = isize(0); i < _layers.size(); ++i)
    {
        auto& l = _layers[i];

        if (l.is_graph())
        {
            if (l.head == l.recorded_head)
                continue;

            auto const chain = impl::walk_chain(*l.graph, l.recorded_head, l.head, options.max_chain_ops);
            if (!chain.found)
            {
                fall_back(chain.reason, layer_at(i));
                return;
            }

            dirty.union_with(impl::change_set_of(chain.ops));
        }
        else
        {
            if (l.direct->version() == l.recorded_version)
                continue;

            dirty.union_with(l.direct->impl_take_changes());
        }
    }

    // Entity granularity is what re-interpretation works at, and it is also what a composition is addressed by.
    dirty.coarsen_to(change_granularity::entity);
    if (dirty.is_everything())
    {
        fall_back(apply_fallback_reason::structure_changed, {});
        return;
    }

    auto const touched = dirty.entities();

    if (stats != nullptr)
    {
        stats->took_fast_path = true;
        stats->composed_entities = touched.size();
    }

    if (touched.empty())
    {
        impl_record_versions();
        return;
    }

    impl_materialize(touched);
    auto const views = impl_views();
    auto const composed = impl::compose(views, touched);

    // Everything about to be re-decided loses what a previous decision recorded, before anything re-decides it.
    report.drop_for_entities(touched);
    report_schema_version_conflicts(report, composed);

    auto builder = document_builder(cc::move(_composed));
    auto unsupported = cc::vector<component_type_id>();

    for (auto const& entity : touched)
    {
        auto const* const raw_entity_at = composed.try_get(entity);
        auto const selection = raw_entity_at == nullptr
                                 ? impl::entity_selection()
                                 : impl::select_entity(entity, *raw_entity_at, policy, report, unsupported);

        auto const was_present = builder.contains_entity(entity);

        if (!selection.instantiate)
        {
            if (was_present)
            {
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
        {
            record_entity(out_changes, entity, change_kind::modified);
        }

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

    // Document-scoped and never retracted, for the same reason an unlayered apply never retracts one.
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
    _composed = cc::move(builder).freeze();
    impl_record_versions();
}

vdoc::layer_handle vdoc::layer_stack::provenance_of(property_path const& path) const
{
    // Top-down, and the first layer that carries the path wins it — which is the same rule the composition applies,
    // asked one path at a time.
    for (auto i = _layers.size() - 1; i >= 0; --i)
    {
        auto const& l = _layers[i];
        if (l.muted)
            continue;

        if (!l.is_graph())
        {
            if (l.direct->document().try_get(path) != nullptr)
                return layer_at(i);

            continue;
        }

        // Materialized on demand for the one entity, rather than keeping every layer's document resident.
        op_id const heads[] = {l.head};
        entity_id const wanted[] = {path.entity};
        auto const raw = l.cache == nullptr ? l.graph->materialize_entities(heads, wanted)
                                            : l.graph->materialize_entities(heads, wanted, *l.cache);

        if (raw.try_get(path) != nullptr)
            return layer_at(i);
    }

    return {};
}
