#include "parse.hh"

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>

using namespace cc::primitive_defines;

namespace
{
using namespace vdoc;

/// Whether `id` is in a list sorted by op id bytes.
[[nodiscard]] bool contains_sorted(cc::span<op_id const> sorted, op_id const& id)
{
    auto lo = isize(0);
    auto hi = sorted.size();
    while (lo < hi)
    {
        auto const mid = lo + (hi - lo) / 2;
        auto const order = sorted[mid].compare_bytes(id);
        if (order == 0)
            return true;

        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

/// What a component's stored `$schema_version` resolves to.
struct resolved_version
{
    i32 version = 0;

    /// The writers disagreed, so the version is unknowable and the component must be skipped.
    bool contested = false;

    /// Several writers agreed, which is a tidy-up hint rather than a problem.
    isize agreed_writers = 0;
};

/// Reads `$schema_version` straight off the raw writers.
///
/// Not through property_reader: a contested version must skip the component rather than be voted on, exactly as a
/// contested `$alive` must not be voted on either.
[[nodiscard]] resolved_version read_schema_version(raw_component const& raw)
{
    auto const* const prop = raw.try_get(reserved::schema_version());
    if (prop == nullptr)
        return {}; // nothing stamped it, which is a set_raw document and not an unknown version

    auto const& writers = prop->writers;
    for (isize i = 1; i < writers.size(); ++i)
        if (writers[i].value != writers[0].value)
            return {.contested = true};

    auto out = resolved_version();
    if (writers[0].value.kind() == value_kind::integer)
        out.version = i32(writers[0].value.as_i64());

    if (writers.size() > 1)
        out.agreed_writers = writers.size();

    return out;
}
} // namespace

bool vdoc::is_alive(raw_component const& raw, property_path const& path, parse_report& report)
{
    auto const* const prop = raw.try_get(reserved::alive());
    if (prop == nullptr)
        return true;

    auto all_false = true;
    for (auto const& w : prop->writers)
        if (w.value.kind() != value_kind::boolean || w.value.as_bool())
        {
            all_false = false;
            break;
        }

    if (all_false)
        return false;

    // Contested only when someone did say false and someone else did not; agreement on alive is silent.
    auto any_false = false;
    for (auto const& w : prop->writers)
        if (w.value.kind() == value_kind::boolean && !w.value.as_bool())
        {
            any_false = true;
            break;
        }

    if (any_false)
        report.diagnostics.push_back(
            {.kind = diagnostic_kind::contested_alive, .path = path, .writer_count = prop->writers.size()});

    return true;
}

vdoc::default_parse_policy vdoc::default_parse_policy::create_with_registry(component_registry const& registry)
{
    auto p = default_parse_policy();
    p._registry = &registry;
    return p;
}

vdoc::default_parse_policy vdoc::default_parse_policy::create_with_local_head(component_registry const& registry,
                                                                              op_graph const& graph,
                                                                              op_id const& local_head)
{
    return create_with_local_heads(registry, graph, cc::span<op_id const>(&local_head, 1));
}

vdoc::default_parse_policy vdoc::default_parse_policy::create_with_local_heads(component_registry const& registry,
                                                                               op_graph const& graph,
                                                                               cc::span<op_id const> local_heads)
{
    auto p = create_with_registry(registry);
    p._local_closure = graph.collect_reachable(local_heads);
    return p;
}

void vdoc::default_parse_policy::extend_local_closure(cc::span<op_id const> new_ops)
{
    if (_local_closure.empty() || new_ops.empty())
        return;

    for (auto const& id : new_ops)
        if (!contains_sorted(_local_closure, id))
            _local_closure.push_back(id);

    cc::sort(_local_closure, op_id::by_bytes{});
}

vdoc::component_schema const* vdoc::default_parse_policy::query_component_schema(component_type_id type) const
{
    return _registry == nullptr ? nullptr : _registry->try_get(type);
}

bool vdoc::default_parse_policy::should_instantiate_entity(entity_id entity, raw_entity const& raw, parse_report& report) const
{
    auto const* const marker = raw.try_get(reserved::entity());
    if (marker == nullptr)
        return true;

    return is_alive(*marker, {.entity = entity, .component = reserved::entity(), .property = reserved::alive()}, report);
}

cc::optional<vdoc::value_view> vdoc::default_parse_policy::resolve_multi_value(property_path const& path,
                                                                               cc::span<property_value const> candidates,
                                                                               parse_report& report) const
{
    CC_ASSERT(candidates.size() >= 2, "a singly-written property is not a conflict");

    // The local user's own write wins, but only when it is the only local one — otherwise there is nothing to prefer.
    if (!_local_closure.empty())
    {
        auto local = isize(-1);
        auto local_count = isize(0);
        for (isize i = 0; i < candidates.size(); ++i)
            if (contains_sorted(_local_closure, candidates[i].writer))
            {
                local = i;
                ++local_count;
            }

        if (local_count == 1)
        {
            report.diagnostics.push_back({.kind = diagnostic_kind::remote_conflict,
                                          .path = path,
                                          .chosen_writer = candidates[local].writer,
                                          .writer_count = candidates.size()});
            return candidates[local].value;
        }
    }

    // Candidates are sorted by writer op id bytes, so the first is the smallest — total and reproducible.
    report.diagnostics.push_back({.kind = diagnostic_kind::multi_valued_conflict,
                                  .path = path,
                                  .chosen_writer = candidates[0].writer,
                                  .writer_count = candidates.size()});
    return candidates[0].value;
}

vdoc::impl::entity_selection vdoc::impl::select_entity(entity_id entity,
                                                       raw_entity const& raw,
                                                       parse_policy const& policy,
                                                       parse_report& report,
                                                       cc::vector<component_type_id>& out_unsupported)
{
    auto out = entity_selection();
    if (!policy.should_instantiate_entity(entity, raw, report))
        return out;

    out.instantiate = true;

    for (auto const& c : raw.components)
    {
        if (c.component == reserved::entity())
            continue;

        auto const* const schema = policy.query_component_schema(c.component);
        if (schema == nullptr)
        {
            // Once per type, not once per occurrence: a large document would otherwise report one per entity.
            auto known = false;
            for (auto const& t : out_unsupported)
                if (t == c.component)
                {
                    known = true;
                    break;
                }

            if (!known)
                out_unsupported.push_back(c.component);

            continue;
        }

        auto const alive_path = property_path{.entity = entity, .component = c.component, .property = reserved::alive()};
        if (!is_alive(c.value, alive_path, report))
            continue;

        auto const version = read_schema_version(c.value);
        if (version.agreed_writers > 0)
            report.agreed_multi_values.push_back(
                {.path = {.entity = entity, .component = c.component, .property = reserved::schema_version()},
                 .writer_count = version.agreed_writers});

        if (version.contested || version.version > schema->current_version)
        {
            report.diagnostics.push_back({.kind = diagnostic_kind::unknown_schema_version,
                                          .path = {.entity = entity, .component = c.component},
                                          .writer_count = version.contested ? isize(2) : isize(1)});
            continue;
        }

        out.components.push_back({.type = c.component, .schema = schema, .raw = &c.value, .version = version.version});
    }

    return out;
}

namespace
{
/// One component that survived selection, and the version its parse will see.
struct candidate
{
    entity_id entity;
    raw_component const* raw = nullptr;
    i32 version = 0;
};

/// Everything selection decided about one component type.
struct pending_column
{
    component_type_id type;
    component_schema const* schema = nullptr;
    cc::vector<candidate> candidates;
};
} // namespace

vdoc::document vdoc::impl::parse_from(cc::span<entity_id const> sorted_entities,
                                      cc::function_ref<raw_entity const*(entity_id)> lookup,
                                      parse_policy const& policy,
                                      parse_report& report)
{
    // Selection walks the entities once, in ascending id order, and decides everything structural:
    // which entities exist, which components survive `$alive`, and which schema versions this build understands.
    //
    // **Every structural diagnostic is filed there, and construction files none.**
    // Construction consumes what selection recorded rather than re-deciding it, which is what makes it impossible for
    // the two to disagree or to double-report.
    auto surviving_entities = cc::vector<entity_id>();
    auto columns = cc::vector<pending_column>();
    auto unsupported = cc::vector<component_type_id>();

    for (auto const& entity : sorted_entities)
    {
        auto const* const raw_entity_at = lookup(entity);
        if (raw_entity_at == nullptr)
            continue;

        auto const selection = impl::select_entity(entity, *raw_entity_at, policy, report, unsupported);
        if (!selection.instantiate)
            continue;

        surviving_entities.push_back(entity);

        for (auto const& sc : selection.components)
        {
            auto found = isize(-1);
            for (isize i = 0; i < columns.size(); ++i)
                if (columns[i].type == sc.type)
                {
                    found = i;
                    break;
                }

            if (found < 0)
            {
                columns.push_back({.type = sc.type, .schema = sc.schema});
                found = columns.size() - 1;
            }

            // Entities are walked in ascending id order, so each column's candidates come out sorted for free.
            columns[found].candidates.push_back({.entity = entity, .raw = sc.raw, .version = sc.version});
        }
    }

    cc::sort(unsupported, component_type_id::by_bytes{});
    for (auto const& t : unsupported)
        report.diagnostics.push_back({.kind = diagnostic_kind::unsupported_component_type, .path = {.component = t}});

    // Columns are built in ascending component type id order, which the per-entity walk above cannot produce.
    cc::sort(columns, [](pending_column const& a, pending_column const& b) { return a.type.compare_bytes(b.type) < 0; });

    auto builder = impl::parser();
    builder.set_entities(surviving_entities);

    for (auto const& column : columns)
    {
        builder.begin_column(*column.schema, column.candidates.size());

        for (auto const& c : column.candidates)
        {
            auto const reader = property_reader::create_for(*c.raw, policy, report, c.entity, column.type, c.version);
            builder.push_component(c.entity, [&](byte* slot) { return column.schema->parse_into(slot, reader); });
        }

        builder.end_column();
    }

    return builder.finish();
}

vdoc::document vdoc::parse(raw_document const& raw, parse_policy const& policy, parse_report& report)
{
    // A raw document's entity vector is already the ascending order parse_from wants, and its own entries are the
    // lookup — so this is the identity case of the general parse rather than a second implementation of it.
    auto entities = cc::vector<entity_id>();
    entities.reserve(raw.entities.size());
    for (auto const& e : raw.entities)
        entities.push_back(e.entity);

    return impl::parse_from(entities, [&](entity_id e) { return raw.try_get(e); }, policy, report);
}
