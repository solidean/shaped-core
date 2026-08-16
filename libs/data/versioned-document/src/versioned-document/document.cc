#include "document.hh"

#include <clean-core/memory/allocation.hh>

using namespace cc::primitive_defines;

/// A bump allocator behind a cc::memory_resource, so a whole document's storage frees in one release.
///
/// **TEMPORARY, and vdoc-local.**
/// This belongs in clean-core as a general bump resource — a `cc::arena_memory_resource` next to
/// `cc::system_memory_resource` — and this copy goes when that lands; `cc::impl::intern_shard` already hand-rolls the
/// same thing privately, which is the second copy that makes it worth having once.
/// See [decisions.md](../../docs/decisions.md#the-document-arena-is-vdoc-local-until-clean-core-grows-one).
///
/// Deallocation is a no-op and there is no in-place resize, so a container backed by this must be created at its final
/// capacity and never grown.
class vdoc::impl::document_arena
{
    // lifetime
public:
    document_arena()
    {
        _resource.allocate_bytes = &allocate_through;
        _resource.deallocate_bytes = &deallocate_through;
        _resource.userdata = this;
    }

    ~document_arena()
    {
        for (auto const& b : _blocks)
            cc::default_memory_resource->deallocate_bytes(b.data, b.size, block_alignment,
                                                          cc::default_memory_resource->userdata);
    }

    document_arena(document_arena const&) = delete;
    document_arena(document_arena&&) = delete;
    document_arena& operator=(document_arena const&) = delete;
    document_arena& operator=(document_arena&&) = delete;

    // allocation
public:
    /// Uninitialized bytes valid until the arena dies.
    /// `alignment` must be a power of two, and `bytes` may be 0.
    [[nodiscard]] byte* allocate(isize bytes, isize alignment)
    {
        CC_ASSERT(bytes >= 0 && alignment > 0, "a bump allocation needs a non-negative size and a positive alignment");
        if (bytes == 0)
            return nullptr;

        if (!_blocks.empty())
        {
            auto& b = _blocks.back();
            auto const aligned = align_up(b.used, alignment);
            if (aligned + bytes <= b.size)
            {
                b.used = aligned + bytes;
                return b.data + aligned;
            }
        }

        // An oversized request gets its own block rather than burning the doubling sequence on it.
        auto const next = _next_block_size < bytes + alignment ? bytes + alignment : _next_block_size;
        add_block(next);
        _next_block_size = next * 2;

        auto& b = _blocks.back();
        auto const aligned = align_up(b.used, alignment);
        b.used = aligned + bytes;
        return b.data + aligned;
    }

    [[nodiscard]] cc::memory_resource const* as_memory_resource() const { return &_resource; }

private:
    struct block
    {
        byte* data = nullptr;
        isize size = 0;
        isize used = 0;
    };

    static constexpr isize block_alignment = 16;

    [[nodiscard]] static isize align_up(isize v, isize alignment) { return (v + alignment - 1) & ~(alignment - 1); }

    void add_block(isize size)
    {
        byte* data = nullptr;
        auto const actual = cc::default_memory_resource->allocate_bytes(&data, size, size, block_alignment,
                                                                        cc::default_memory_resource->userdata);
        _blocks.push_back({.data = data, .size = actual, .used = 0});
    }

    static isize allocate_through(byte** out_ptr, isize min_bytes, isize max_bytes, isize alignment, void* userdata)
    {
        if (min_bytes == 0)
        {
            *out_ptr = nullptr;
            return 0;
        }

        // The arena hands out exactly what was asked for; there is no size class to round up into.
        (void)max_bytes;
        *out_ptr = static_cast<document_arena*>(userdata)->allocate(min_bytes, alignment);
        return min_bytes;
    }

    static void deallocate_through(byte*, isize, isize, void*) {}

    cc::vector<block> _blocks;
    isize _next_block_size = 16 * 1024;
    cc::memory_resource _resource = {};
};

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
    _columns(cc::move(rhs._columns))
{
    rhs._entities = nullptr;
    rhs._entity_count = 0;
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
    _columns = cc::move(rhs._columns);

    rhs._entities = nullptr;
    rhs._entity_count = 0;
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
                         .count = 0});

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
