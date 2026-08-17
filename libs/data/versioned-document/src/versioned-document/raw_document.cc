#include "raw_document.hh"

#include <clean-core/common/assert.hh>

using namespace cc::primitive_defines;

namespace
{
/// Binary search over a level sorted by canonical id bytes.
/// Every level is sorted the same way, so one helper serves all three.
template <class EntryT, class IdT>
[[nodiscard]] EntryT const* find_sorted(cc::vector<EntryT> const& entries, IdT id, auto&& id_of)
{
    auto lo = isize(0);
    auto hi = entries.size();
    while (lo < hi)
    {
        auto const mid = lo + (hi - lo) / 2;
        auto const order = id_of(entries[mid]).compare_bytes(id);
        if (order == 0)
            return &entries[mid];

        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}
} // namespace

vdoc::value_view vdoc::raw_property::single() const
{
    CC_ASSERT(writers.size() == 1, "single() on a property that is not singly-written; check is_multi_valued() first");
    return writers[0].value;
}

vdoc::raw_property const* vdoc::raw_component::try_get(property_id property) const
{
    auto const* const e = find_sorted(properties, property, [](entry const& x) { return x.property; });
    return e ? &e->value : nullptr;
}

vdoc::raw_component const* vdoc::raw_entity::try_get(component_type_id component) const
{
    auto const* const e = find_sorted(components, component, [](entry const& x) { return x.component; });
    return e ? &e->value : nullptr;
}

vdoc::raw_entity const* vdoc::raw_document::try_get(entity_id entity) const
{
    auto const* const e = find_sorted(entities, entity, [](entry const& x) { return x.entity; });
    return e ? &e->value : nullptr;
}

vdoc::raw_property const* vdoc::raw_document::try_get(property_path const& path) const
{
    auto const* const e = try_get(path.entity);
    if (e == nullptr)
        return nullptr;

    auto const* const c = e->try_get(path.component);
    if (c == nullptr)
        return nullptr;

    return c->try_get(path.property);
}

isize vdoc::raw_document::property_count() const
{
    auto count = isize(0);
    for (auto const& e : entities)
        for (auto const& c : e.value.components)
            count += c.value.properties.size();

    return count;
}
