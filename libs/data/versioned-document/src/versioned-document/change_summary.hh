#pragma once

#include <clean-core/container/vector.hh>
#include <versioned-document/ids.hh>

/// What evolving a document changed, as an output rather than something a caller diffs for itself.

/// Whether a thing was added, removed, or re-parsed in place.
enum class vdoc::change_kind : vdoc::u8
{
    added,
    removed,

    /// The component was re-parsed because a raw property under it changed.
    ///
    /// **Not a claim that the parsed value differs**, which the library cannot make: a component is required to be
    /// move-constructible and destructible, and nothing more, so there is no equality to test.
    /// An application that needs value-level change detection compares what it already keeps.
    modified,
};

/// What one apply did, so an application can invalidate its own projections without diffing two documents.
///
/// Sorted, like everything else here, so two summaries compare directly and an application gets a reproducible order.
struct vdoc::change_summary
{
    struct entity_change
    {
        entity_id entity;
        change_kind kind = change_kind::modified;

        [[nodiscard]] friend bool operator==(entity_change const&, entity_change const&) = default;
    };

    struct component_change
    {
        entity_id entity;
        component_type_id component;
        change_kind kind = change_kind::modified;

        [[nodiscard]] friend bool operator==(component_change const&, component_change const&) = default;
    };

    /// Sorted by entity id bytes.
    cc::vector<entity_change> entities;

    /// Sorted by entity id bytes, then component type id bytes.
    cc::vector<component_change> components;

    [[nodiscard]] bool is_empty() const { return entities.empty() && components.empty(); }

    void clear()
    {
        entities.clear();
        components.clear();
    }
};
