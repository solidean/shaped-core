#pragma once

#include <clean-core/container/vector.hh>
#include <versioned-document/ids.hh>
#include <versioned-document/op.hh>

/// The materialized but still untyped document: the conceptual model, with no idea what any of it means.
///
///   raw_document  : entity_id         -> raw_entity
///   raw_entity    : component_type_id -> raw_component
///   raw_component : property_id       -> raw_property
///
/// **A raw document borrows the graph's op bytes.** Every value_view in it points into the payload of the op that
/// wrote it, so a raw document is valid only while those ops are still in the op_graph it came from.
///
/// Each level is a distinct struct wrapping a vector sorted by canonical id bytes, so lookup is a binary search and
/// iteration order is the same on every machine — the levels cannot be interchanged, and nothing here is a hash
/// container whose order could leak into output.
///
/// The design is [the concept](../../docs/concept.md#interpretation) and [multi-values](../../docs/concept.md#multi-values).

/// One surviving write of a property: the value, and the op that wrote it.
struct vdoc::property_value
{
    op_id writer;
    value_view value;
};

/// Every surviving write of one property.
///
/// One value is the normal case.
/// **More than one means concurrent writers where neither dominates the other**, which is not an error and not
/// something storage resolves — it is what happened, and choosing between them is a parse-layer decision.
///
/// Two writers with byte-identical values still leave two entries.
struct vdoc::raw_property
{
    /// Sorted by writer op id bytes, so the order is reproducible across runs and machines.
    cc::vector<property_value> writers;

    [[nodiscard]] bool is_multi_valued() const { return writers.size() > 1; }

    /// The value, when exactly one writer survived.
    /// Asserts on a multi-valued property, because picking one silently is the parse layer's job and never storage's.
    [[nodiscard]] value_view single() const;
};

/// One component's properties, sorted by property id bytes.
struct vdoc::raw_component
{
    struct entry
    {
        property_id property;
        raw_property value;
    };

    cc::vector<entry> properties;

    [[nodiscard]] raw_property const* try_get(property_id property) const;
};

/// One entity's components, sorted by component type id bytes.
struct vdoc::raw_entity
{
    struct entry
    {
        component_type_id component;
        raw_component value;
    };

    cc::vector<entry> components;

    [[nodiscard]] raw_component const* try_get(component_type_id component) const;
};

/// The whole materialized document, sorted by entity id bytes.
struct vdoc::raw_document
{
    struct entry
    {
        entity_id entity;
        raw_entity value;
    };

    cc::vector<entry> entities;

    [[nodiscard]] raw_entity const* try_get(entity_id entity) const;

    /// The surviving writes of one path, or null if nothing ever wrote it.
    [[nodiscard]] raw_property const* try_get(property_path const& path) const;

    /// How many property paths carry at least one write.
    [[nodiscard]] isize property_count() const;
};
