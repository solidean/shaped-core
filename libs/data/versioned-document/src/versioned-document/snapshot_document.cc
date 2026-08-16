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
} // namespace

vdoc::snapshot_document vdoc::snapshot_document::create_owning_copy(raw_document const& doc)
{
    auto result = snapshot_document();
    result._arena.resize_to_uninitialized(total_value_bytes(doc));

    auto at = isize(0);
    auto* const arena = result._arena.data();

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

    CC_ASSERT(at == result._arena.size(), "the sizing pass and the fill pass disagreed");
    return result;
}

vdoc::snapshot_document vdoc::snapshot_document::create_from_owned_arena(cc::vector<byte> arena, raw_document doc)
{
    auto result = snapshot_document();
    result._arena = cc::move(arena);
    result._document = cc::move(doc);
    return result;
}
