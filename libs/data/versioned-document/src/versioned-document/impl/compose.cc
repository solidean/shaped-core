#include "compose.hh"

#include <clean-core/algorithm/search.hh>
#include <clean-core/algorithm/sort.hh>
#include <versioned-document/component.hh> // reserved::schema_version / is_reserved

using namespace cc::primitive_defines;

namespace
{
using namespace vdoc;

/// One layer's contribution to one property, plus which layer it came from.
struct contribution
{
    component_type_id component;
    property_id property;
    raw_property const* writers = nullptr;

    /// 0 is the topmost layer, so sorting ascending puts the winner of each path first in its run.
    i32 layer_rank = 0;
};

[[nodiscard]] bool before(contribution const& a, contribution const& b)
{
    if (auto const by_component = a.component.compare_bytes(b.component); by_component != 0)
        return by_component < 0;
    if (auto const by_property = a.property.compare_bytes(b.property); by_property != 0)
        return by_property < 0;
    return a.layer_rank < b.layer_rank;
}

/// Whether the layers that supplied real data to this component disagree about its `$schema_version`.
///
/// `run` is one component's contributions, sorted, so the winning stamp is the first `$schema_version` entry in it.
/// A layer with no stamp has no opinion; a layer with one that supplied no real property has an opinion only insofar as
/// its stamp may have won, which the comparison against the winner already covers.
[[nodiscard]] bool schema_versions_disagree(cc::span<contribution const> run)
{
    auto const version_property = reserved::schema_version();

    auto winning = cc::optional<value_view>();
    for (auto const& e : run)
        if (e.property == version_property && !e.writers->is_multi_valued())
        {
            winning = e.writers->single();
            break;
        }

    if (!winning.has_value())
        return false;

    for (auto const& e : run)
    {
        if (!(e.property == version_property) || e.writers->is_multi_valued())
            continue;

        if (e.writers->single() == winning.value())
            continue;

        // A differing stamp only matters from a layer that actually supplied data here.
        for (auto const& other : run)
            if (other.layer_rank == e.layer_rank && !reserved::is_reserved(other.property.as_string_view()))
                return true;
    }

    return false;
}

/// Composes one entity into `out`, reusing `scratch` for the contribution list.
/// Appends any component dropped over a `$schema_version` disagreement to `out_conflicts`.
void compose_one(cc::span<impl::layer_view const> layers,
                 entity_id entity,
                 cc::vector<contribution>& scratch,
                 raw_entity& out,
                 cc::vector<property_path>& out_conflicts)
{
    scratch.clear();

    // One binary search per layer, and layers are few — this is where the O(layers) in the cost model lives.
    // `layers` is bottom-first, so the rank counts down for 0 to mean the top.
    auto const layer_count = i32(layers.size());
    for (auto i = isize(0); i < layers.size(); ++i)
    {
        auto const* const doc = layers[i].document;
        if (doc == nullptr)
            continue;

        auto const* const raw = doc->try_get(entity);
        if (raw == nullptr)
            continue;

        auto const rank = layer_count - 1 - i32(i);
        for (auto const& c : raw->components)
            for (auto const& p : c.value.properties)
                scratch.push_back(
                    {.component = c.component, .property = p.property, .writers = &p.value, .layer_rank = rank});
    }

    // Sorting rather than merging the layers pairwise: the rank is part of the key, so the winner of every path is
    // simply the first entry of its run, and the sort needs no stability guarantee.
    cc::sort(scratch, before);

    // One component's contributions are contiguous, so each run is checked and emitted in one pass.
    auto run_begin = isize(0);
    while (run_begin < scratch.size())
    {
        auto run_end = run_begin;
        while (run_end < scratch.size() && scratch[run_end].component == scratch[run_begin].component)
            ++run_end;

        auto const run = cc::span<contribution const>(scratch.data() + run_begin, run_end - run_begin);

        if (schema_versions_disagree(run))
        {
            // Dropped rather than misparsed: reading v2 properties as v1, or vanishing behind an unknown version, are
            // both worse than saying the layers disagree.
            out_conflicts.push_back({.entity = entity, .component = scratch[run_begin].component});
            run_begin = run_end;
            continue;
        }

        for (auto i = isize(0); i < run.size(); ++i)
        {
            auto const& e = run[i];

            // Anything after the first of a path's run is a lower layer losing that path outright.
            if (i > 0 && run[i - 1].property == e.property)
                continue;

            if (out.components.empty() || !(out.components.back().component == e.component))
                out.components.push_back(raw_entity::entry{.component = e.component, .value = {}});

            out.components.back().value.properties.push_back(
                raw_component::entry{.property = e.property, .value = *e.writers});
        }

        run_begin = run_end;
    }
}
} // namespace

vdoc::raw_entity const* vdoc::impl::composed_document::try_get(entity_id entity) const
{
    auto const at = cc::find_in_sorted(entities, entity, entity_id::by_bytes{});
    if (!at.has_value())
        return nullptr;

    auto const& out = composed[at.value()];
    return out.components.empty() ? nullptr : &out;
}

vdoc::impl::composed_document vdoc::impl::compose(cc::span<layer_view const> layers, cc::span<entity_id const> entities)
{
    auto out = composed_document();
    out.entities = cc::vector<entity_id>::create_copy_of(entities);
    out.composed.resize_to_defaulted(entities.size());

    auto scratch = cc::vector<contribution>();
    for (auto i = isize(0); i < entities.size(); ++i)
        compose_one(layers, entities[i], scratch, out.composed[i], out.schema_version_conflicts);

    return out;
}

cc::vector<vdoc::entity_id> vdoc::impl::compose_entities(cc::span<layer_view const> layers)
{
    auto out = cc::vector<entity_id>();
    for (auto const& layer : layers)
    {
        if (layer.document == nullptr)
            continue;

        for (auto const& e : layer.document->entities)
            out.push_back(e.entity);
    }

    cc::sort(out, entity_id::by_bytes{});

    auto kept = isize(0);
    for (auto const& e : out)
    {
        if (kept > 0 && out[kept - 1] == e)
            continue;

        out[kept] = e;
        ++kept;
    }

    out.resize_down_to(kept);
    return out;
}
