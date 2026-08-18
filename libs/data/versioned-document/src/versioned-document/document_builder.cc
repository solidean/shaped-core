#include "document_builder.hh"

#include "impl/document_arena.hh"

#include <clean-core/algorithm/search.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>

using namespace cc::primitive_defines;

using vdoc::component_type_id;
using vdoc::entity_id;
using vdoc::impl::component_column;

namespace
{
/// Where `entity` is, or would go, in a sorted entity array.
[[nodiscard]] isize slot_for(entity_id const* entities, isize count, entity_id entity)
{
    auto lo = isize(0);
    auto hi = count;
    while (lo < hi)
    {
        auto const mid = lo + (hi - lo) / 2;
        if (entities[mid].compare_bytes(entity) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

[[nodiscard]] bool occupies(entity_id const* entities, isize count, isize at, entity_id entity)
{
    return at < count && entities[at] == entity;
}

/// The next capacity for an array that needs to hold `needed`.
[[nodiscard]] isize grown(isize needed)
{
    return cc::max(isize(8), needed + needed / 2);
}

/// Shifts `[at, count)` one slot up, leaving `at` free.
void open_slot(entity_id* entities, isize count, isize at)
{
    for (auto i = count; i > at; --i)
        entities[i] = entities[i - 1];
}

/// Shifts `[at + 1, count)` one slot down, closing over `at`.
void close_slot(entity_id* entities, isize count, isize at)
{
    for (auto i = at; i + 1 < count; ++i)
        entities[i] = entities[i + 1];
}

/// What one slot of a column costs across both of its arrays.
[[nodiscard]] isize slot_bytes(component_column const& column)
{
    return isize(sizeof(entity_id)) + column.schema.component_size;
}
} // namespace

vdoc::document_builder::document_builder(document&& doc) : _doc(cc::move(doc))
{
    // A document that never went through a parse has no arena, and the builder allocates from one on its first edit.
    if (_doc._arena == nullptr)
        _doc._arena = cc::make_unique<impl::document_arena>();
}

bool vdoc::document_builder::insert_entity(entity_id entity)
{
    auto const at = slot_for(_doc._entities, _doc._entity_count, entity);
    if (occupies(_doc._entities, _doc._entity_count, at, entity))
        return false;

    auto& arena = *_doc._arena;

    if (_doc._entity_count == _doc._entity_capacity)
    {
        auto const capacity = grown(_doc._entity_count + 1);
        auto* const fresh = reinterpret_cast<entity_id*>(
            arena.allocate(capacity * isize(sizeof(entity_id)), isize(alignof(entity_id))));

        // An entity id is a handle, so the two halves copy rather than relocate.
        for (isize i = 0; i < at; ++i)
            fresh[i] = _doc._entities[i];
        for (auto i = at; i < _doc._entity_count; ++i)
            fresh[i + 1] = _doc._entities[i];

        arena.note_dead(_doc._entity_capacity * isize(sizeof(entity_id)));
        _doc._entities = fresh;
        _doc._entity_capacity = capacity;
    }
    else
    {
        open_slot(_doc._entities, _doc._entity_count, at);
    }

    _doc._entities[at] = entity;
    ++_doc._entity_count;
    return true;
}

bool vdoc::document_builder::remove_entity(entity_id entity)
{
    auto const at = slot_for(_doc._entities, _doc._entity_count, entity);
    if (!occupies(_doc._entities, _doc._entity_count, at, entity))
        return false;

    // Every component goes with it: `entities()` and the columns must never disagree about what exists.
    // Walked backwards, so a column emptied by the removal cannot shift an index still to be visited.
    for (auto i = _doc._columns.size(); i > 0; --i)
        (void)remove_component(_doc._columns[i - 1].schema.type, entity);

    close_slot(_doc._entities, _doc._entity_count, at);
    --_doc._entity_count;
    return true;
}

vdoc::change_kind vdoc::document_builder::set_component(component_schema const& schema,
                                                        entity_id entity,
                                                        cc::function_ref<bool(byte* slot)> construct)
{
    CC_ASSERT(_doc.contains(entity), "set_component on an entity the document does not have - insert it first");
    CC_ASSERT(schema.relocate_range != nullptr, "a component schema without relocate_range cannot back a dense column");

    auto const index = impl_column_for(schema);
    {
        auto& column = _doc._columns[index];
        auto const at = slot_for(column.entities, column.count, entity);

        if (occupies(column.entities, column.count, at, entity))
        {
            auto* const slot = column.components + at * schema.component_size;

            // Destroyed first, so `construct` sees uninitialized storage exactly as a parse does.
            schema.destroy_range(slot, 1);
            if (construct(slot))
                return change_kind::modified;

            // Declining is how a parse says "drop this component", and it means the same here.
            schema.relocate_range(slot, slot + schema.component_size, column.count - at - 1);
            close_slot(column.entities, column.count, at);
            --column.count;
            impl_drop_if_empty(index);
            return change_kind::removed;
        }
    }

    // Growing invalidates the column's arrays, so nothing above may be held across it.
    impl_reserve_one(_doc._columns[index]);

    auto& column = _doc._columns[index];
    auto const at = slot_for(column.entities, column.count, entity);
    auto* const slot = column.components + at * schema.component_size;

    // The hole is opened BEFORE constructing, so `construct` runs exactly once and writes where the component ends up.
    schema.relocate_range(slot + schema.component_size, slot, column.count - at);

    if (!construct(slot))
    {
        // Nothing was built, so the tail goes back and the document is exactly as it was.
        schema.relocate_range(slot, slot + schema.component_size, column.count - at);
        impl_drop_if_empty(index);
        return change_kind::removed;
    }

    open_slot(column.entities, column.count, at);
    column.entities[at] = entity;
    ++column.count;
    return change_kind::added;
}

bool vdoc::document_builder::remove_component(component_type_id type, entity_id entity)
{
    auto const index
        = cc::first_at_least_in_sorted(_doc._columns, type, [](component_column const& c, component_type_id const& t)
                                       { return c.schema.type.compare_bytes(t) < 0; });

    if (index >= _doc._columns.size() || !(_doc._columns[index].schema.type == type))
        return false;

    auto& column = _doc._columns[index];
    auto const at = slot_for(column.entities, column.count, entity);
    if (!occupies(column.entities, column.count, at, entity))
        return false;

    auto* const slot = column.components + at * column.schema.component_size;
    column.schema.destroy_range(slot, 1);
    column.schema.relocate_range(slot, slot + column.schema.component_size, column.count - at - 1);

    close_slot(column.entities, column.count, at);
    --column.count;
    impl_drop_if_empty(index);
    return true;
}

bool vdoc::document_builder::contains_entity(entity_id entity) const
{
    return _doc.contains(entity);
}

bool vdoc::document_builder::has_component(component_type_id type, entity_id entity) const
{
    return _doc.has_component(type, entity);
}

cc::vector<component_type_id> vdoc::document_builder::component_types() const
{
    return _doc.component_types();
}

isize vdoc::document_builder::impl_column_for(component_schema const& schema)
{
    auto const at = cc::first_at_least_in_sorted(_doc._columns, schema.type,
                                                 [](component_column const& c, component_type_id const& t)
                                                 { return c.schema.type.compare_bytes(t) < 0; });

    if (at < _doc._columns.size() && _doc._columns[at].schema.type == schema.type)
    {
        CC_ASSERT(_doc._columns[at].schema.type_key == schema.type_key, "two C++ types are registered under one "
                                                                        "component type name");
        return at;
    }

    // An empty column is a legal intermediate state here, and impl_drop_if_empty removes it again if nothing lands.
    _doc._columns.insert_at(at, component_column{.schema = schema});

    return at;
}

void vdoc::document_builder::impl_reserve_one(component_column& column)
{
    if (column.count < column.capacity)
        return;

    auto& arena = *_doc._arena;
    auto const capacity = grown(column.count + 1);

    auto* const ids
        = reinterpret_cast<entity_id*>(arena.allocate(capacity * isize(sizeof(entity_id)), isize(alignof(entity_id))));
    auto* const components = arena.allocate(capacity * column.schema.component_size, column.schema.component_align);

    for (isize i = 0; i < column.count; ++i)
        ids[i] = column.entities[i];

    column.schema.relocate_range(components, column.components, column.count);
    arena.note_dead(column.capacity * slot_bytes(column));

    column.entities = ids;
    column.components = components;
    column.capacity = capacity;
}

void vdoc::document_builder::impl_drop_if_empty(isize index)
{
    if (_doc._columns[index].count > 0)
        return;

    _doc._arena->note_dead(_doc._columns[index].capacity * slot_bytes(_doc._columns[index]));

    _doc._columns.remove_at(index);
}

void vdoc::document_builder::compact()
{
    auto fresh = cc::make_unique<impl::document_arena>();

    auto* const entities = reinterpret_cast<entity_id*>(
        fresh->allocate(_doc._entity_count * isize(sizeof(entity_id)), isize(alignof(entity_id))));
    for (isize i = 0; i < _doc._entity_count; ++i)
        entities[i] = _doc._entities[i];

    for (auto& column : _doc._columns)
    {
        auto* const ids = reinterpret_cast<entity_id*>(
            fresh->allocate(column.count * isize(sizeof(entity_id)), isize(alignof(entity_id))));
        auto* const components
            = fresh->allocate(column.count * column.schema.component_size, column.schema.component_align);

        for (isize i = 0; i < column.count; ++i)
            ids[i] = column.entities[i];

        // Moved rather than re-parsed: the values are already decoded, and a component owes only being
        // move-constructible.
        column.schema.relocate_range(components, column.components, column.count);

        column.entities = ids;
        column.components = components;
        column.capacity = column.count;
    }

    _doc._entities = entities;
    _doc._entity_capacity = _doc._entity_count;

    // Released last, because everything above still read out of it.
    _doc._arena = cc::move(fresh);
}

isize vdoc::document_builder::dead_arena_bytes() const
{
    return _doc._arena->dead_bytes();
}

isize vdoc::document_builder::live_arena_bytes() const
{
    return _doc._arena->live_bytes();
}

vdoc::document vdoc::document_builder::freeze() &&
{
    return cc::move(_doc);
}
