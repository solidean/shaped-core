#include "document.hh"

#include "impl/document_arena.hh"

using namespace cc::primitive_defines;

isize vdoc::impl::component_column::index_of(entity_id entity) const
{
    auto lo = isize(0);
    auto hi = count;
    while (lo < hi)
    {
        auto const mid = lo + (hi - lo) / 2;
        auto const order = entities[mid].compare_bytes(entity);
        if (order == 0)
            return mid;

        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

vdoc::document::document() = default;

vdoc::document::~document()
{
    destroy_components();
}

vdoc::document::document(document&& rhs) noexcept
  : _arena(cc::move(rhs._arena)),
    _entities(rhs._entities),
    _entity_count(rhs._entity_count),
    _entity_capacity(rhs._entity_capacity),
    _columns(cc::move(rhs._columns))
{
    rhs._entities = nullptr;
    rhs._entity_count = 0;
    rhs._entity_capacity = 0;
}

vdoc::document& vdoc::document::operator=(document&& rhs) noexcept
{
    if (this == &rhs)
        return *this;

    // The components go before the arena they live in, which is exactly what the destructor does.
    destroy_components();

    _arena = cc::move(rhs._arena);
    _entities = rhs._entities;
    _entity_count = rhs._entity_count;
    _entity_capacity = rhs._entity_capacity;
    _columns = cc::move(rhs._columns);

    rhs._entities = nullptr;
    rhs._entity_count = 0;
    rhs._entity_capacity = 0;
    return *this;
}

void vdoc::document::destroy_components()
{
    for (auto const& c : _columns)
        if (c.count > 0)
            c.schema.destroy_range(c.components, c.count);

    _columns.clear();
    _entities = nullptr;
    _entity_count = 0;
    _entity_capacity = 0;
}

bool vdoc::document::contains(entity_id entity) const
{
    auto lo = isize(0);
    auto hi = _entity_count;
    while (lo < hi)
    {
        auto const mid = lo + (hi - lo) / 2;
        auto const order = _entities[mid].compare_bytes(entity);
        if (order == 0)
            return true;

        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

cc::vector<vdoc::component_type_id> vdoc::document::component_types() const
{
    auto out = cc::vector<component_type_id>();
    out.reserve(_columns.size());
    for (auto const& c : _columns)
        out.push_back(c.schema.type);

    return out;
}

bool vdoc::document::has_component(component_type_id type, entity_id entity) const
{
    auto const* const c = try_find_column(type);
    return c != nullptr && c->index_of(entity) >= 0;
}

isize vdoc::document::count_of(component_type_id type) const
{
    auto const* const c = try_find_column(type);
    return c == nullptr ? 0 : c->count;
}

vdoc::impl::component_column const* vdoc::document::try_find_column(component_type_id type) const
{
    auto lo = isize(0);
    auto hi = _columns.size();
    while (lo < hi)
    {
        auto const mid = lo + (hi - lo) / 2;
        auto const order = _columns[mid].schema.type.compare_bytes(type);
        if (order == 0)
            return &_columns[mid];

        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}

vdoc::impl::parser::parser()
{
    _doc._arena = cc::make_unique<document_arena>();
}

vdoc::impl::parser::~parser() = default;

vdoc::impl::document_arena& vdoc::impl::parser::arena() const
{
    return *_doc._arena;
}

void vdoc::impl::parser::set_entities(cc::span<entity_id const> entities)
{
    CC_ASSERT(_doc._entities == nullptr, "the entity table is set once");

    auto* const storage = reinterpret_cast<entity_id*>(
        _doc._arena->allocate(entities.size() * isize(sizeof(entity_id)), isize(alignof(entity_id))));

    for (isize i = 0; i < entities.size(); ++i)
    {
        CC_ASSERT(i == 0 || std::is_lt(entities[i - 1].compare_bytes(entities[i])), "the entity table must be sorted "
                                                                                    "by id bytes");
        storage[i] = entities[i];
    }

    _doc._entities = storage;
    _doc._entity_count = entities.size();
    _doc._entity_capacity = entities.size();
}

void vdoc::impl::parser::begin_column(component_schema const& schema, isize capacity)
{
    CC_ASSERT(_open_column < 0, "a column is already open");
    CC_ASSERT(capacity > 0, "an empty column is not opened at all");
    CC_ASSERT(_doc._columns.empty() || std::is_lt(_doc._columns.back().schema.type.compare_bytes(schema.type)),
              "columns are added in ascending component type id order");

    _open_entities = reinterpret_cast<entity_id*>(
        _doc._arena->allocate(capacity * isize(sizeof(entity_id)), isize(alignof(entity_id))));

    _doc._columns.push_back(
        component_column{.schema = schema,
                         .entities = _open_entities,
                         .components = _doc._arena->allocate(capacity * schema.component_size, schema.component_align),
                         .count = 0,
                         .capacity = capacity});

    _open_column = _doc._columns.size() - 1;
    _open_capacity = capacity;
}

bool vdoc::impl::parser::push_component(entity_id entity, cc::function_ref<bool(byte* slot)> construct)
{
    CC_ASSERT(_open_column >= 0, "no column is open");

    auto& column = _doc._columns[_open_column];
    CC_ASSERT(column.count < _open_capacity, "the column was opened with too small a capacity");
    CC_ASSERT(column.count == 0 || std::is_lt(_open_entities[column.count - 1].compare_bytes(entity)),
              "a column's entities must be pushed in ascending id order");

    if (!construct(column.components + column.count * column.schema.component_size))
        return false;

    _open_entities[column.count] = entity;
    ++column.count;
    return true;
}

void vdoc::impl::parser::end_column()
{
    CC_ASSERT(_open_column >= 0, "no column is open");

    // A column that kept nothing is not a column; the arena keeps its bytes either way.
    if (_doc._columns[_open_column].count == 0)
        _doc._columns.remove_back();

    _open_column = -1;
    _open_capacity = 0;
    _open_entities = nullptr;
}

vdoc::document vdoc::impl::parser::finish()
{
    CC_ASSERT(_open_column < 0, "a column is still open");

    auto out = cc::move(_doc);
    _doc = document();
    _doc._arena = cc::make_unique<document_arena>();
    return out;
}
