#include <clean-core/algorithm/search.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <versioned-document/snapshot_document.hh>

using namespace cc::primitive_defines;

/// Copying a raw document into an arena is two passes, and it must stay two passes.
/// Sizing first and filling second is what keeps every value_view valid: a single append during the fill would
/// reallocate the arena and dangle every view written before it.

namespace
{
/// The total size of every writer's value bytes, which is exactly what the arena must hold.
[[nodiscard]] isize total_value_bytes(vdoc::raw_document const& doc)
{
    auto total = isize(0);
    for (auto const& e : doc.entities)
        for (auto const& c : e.value.components)
            for (auto const& p : c.value.properties)
                for (auto const& w : p.value.writers)
                    total += w.value.bytes().size();

    return total;
}

/// Inserts `value` at `at`, shifting the tail up by one.
///
/// clean-core's vector has no insert-at-position yet — container/vector.hh tracks it as a TODO — so this is the local
/// stand-in, and the place to delete once it grows one.
template <class T>
void insert_at(cc::vector<T>& values, isize at, T value)
{
    values.push_back(cc::move(value));
    for (auto i = values.size() - 1; i > at; --i)
        cc::swap(values[i], values[i - 1]);
}

/// The entry keyed by `id`, inserted in sorted position where absent.
/// Every level of a raw_document is a vector of `{id, value}` sorted by the id's canonical bytes, so one helper covers
/// all three.
template <class EntryT, class IdT>
[[nodiscard]] EntryT& entry_for(cc::vector<EntryT>& entries, IdT EntryT::* key, IdT id)
{
    auto const at = cc::first_at_least_in_sorted(
        entries, id, [key](EntryT const& e, IdT const& k) { return (e.*key).compare_bytes(k) < 0; });

    if (at < entries.size() && entries[at].*key == id)
        return entries[at];

    insert_at(entries, at, EntryT{});
    entries[at].*key = id;
    return entries[at];
}
} // namespace

vdoc::snapshot_document vdoc::snapshot_document::create_owning_copy(raw_document const& doc)
{
    auto result = snapshot_document();

    // One chunk, sized exactly: the load path is unchanged by chunking, and a snapshot that is never advanced keeps
    // exactly the shape it always had.
    auto arena_bytes = cc::vector<byte>();
    arena_bytes.resize_to_uninitialized(total_value_bytes(doc));
    result._owned_bytes = arena_bytes.size();

    auto at = isize(0);
    auto* const arena = arena_bytes.data();

    // The shape is copied level by level rather than by assigning the vectors, because every value_view has to be
    // rebuilt against the arena instead of pointing back at the ops.
    result._document.entities.reserve_exact(doc.entities.size());
    for (auto const& e : doc.entities)
    {
        auto entity = raw_document::entry{.entity = e.entity};
        entity.value.components.reserve_exact(e.value.components.size());

        for (auto const& c : e.value.components)
        {
            auto component = raw_entity::entry{.component = c.component};
            component.value.properties.reserve_exact(c.value.properties.size());

            for (auto const& p : c.value.properties)
            {
                auto property = raw_component::entry{.property = p.property};
                property.value.writers.reserve_exact(p.value.writers.size());

                for (auto const& w : p.value.writers)
                {
                    auto const bytes = w.value.bytes();
                    cc::memcpy(arena + at, bytes.data(), size_t(bytes.size()));
                    property.value.writers.push_back(
                        {.writer = w.writer,
                         .value = value_view::from_validated_bytes(cc::span<byte const>(arena + at, bytes.size()))});
                    at += bytes.size();
                }

                component.value.properties.push_back(cc::move(property));
            }

            entity.value.components.push_back(cc::move(component));
        }

        result._document.entities.push_back(cc::move(entity));
    }

    CC_ASSERT(at == arena_bytes.size(), "the sizing pass and the fill pass disagreed");

    // Moved in only now, so nothing above could have reallocated it.
    // The move is a steal, so every view written above keeps pointing at the same bytes.
    result._chunks.push_back(cc::move(arena_bytes));
    return result;
}

vdoc::snapshot_document vdoc::snapshot_document::create_from_owned_arena(cc::vector<byte> arena, raw_document doc)
{
    auto result = snapshot_document();
    result._owned_bytes = arena.size();
    result._chunks.push_back(cc::move(arena));
    result._document = cc::move(doc);
    return result;
}

vdoc::value_view vdoc::snapshot_document::impl_append(cc::span<byte const> bytes)
{
    // A chunk is reserved once and filled up to that reservation, never past it: every view already handed out points
    // into it, and a reallocation would dangle all of them together.
    auto const need = bytes.size();
    if (_chunks.empty() || _chunks.back().size() + need > _chunks.back().capacity())
    {
        auto chunk = cc::vector<byte>();
        chunk.reserve_exact(cc::max(isize(4096), need));
        _chunks.push_back(cc::move(chunk));
    }

    auto& chunk = _chunks.back();
    auto const at = chunk.size();
    chunk.resize_to_uninitialized(at + need);
    cc::memcpy(chunk.data() + at, bytes.data(), size_t(need));
    _owned_bytes += need;

    return value_view::from_validated_bytes(cc::span<byte const>(chunk.data() + at, need));
}

void vdoc::snapshot_document::set_single_writer(property_path const& path, op_id const& writer, cc::span<byte const> bytes)
{
    auto& entity = entry_for(_document.entities, &raw_document::entry::entity, path.entity);
    auto& component = entry_for(entity.value.components, &raw_entity::entry::component, path.component);
    auto& property = entry_for(component.value.properties, &raw_component::entry::property, path.property);

    // The bytes the old writers pointed at are still in a chunk and can never be reclaimed in place, so they are
    // counted instead: a caller rebuilds once they outweigh the live ones.
    for (auto const& w : property.value.writers)
        _dead_bytes += w.value.bytes().size();

    property.value.writers.clear();
    property.value.writers.push_back({.writer = writer, .value = impl_append(bytes)});
}
