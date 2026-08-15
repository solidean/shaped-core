#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <versioned-document/op.hh>
#include <versioned-document/op_graph.hh>

/// Builds one op from a set of edits, diffing against its own parents so only changed properties are written.
///
/// **Pure: the builder holds no graph state of its own.**
/// It is a staging area for edits, and the graph only enters at build(), which is where the diff happens.
///
/// This is what keeps history small enough to keep forever, and it is why an op's assignment list is a genuine
/// changelog rather than a snapshot.
///
/// The design is [the concept](../../docs/concept.md#ops-write-only-what-changed).
class vdoc::op_builder
{
public:
    /// The parents this op extends or merges.
    /// Sorted and deduplicated at build time, so the caller's order never reaches the hash.
    op_builder& set_parents(cc::span<op_id const> parents);

    /// Free-form, informational metadata — author, timestamp, description.
    /// Committed to by the hash, so it cannot be altered after the fact, but nothing interprets it.
    op_builder& set_metadata(value metadata);

    /// Stages one property write.
    ///
    /// Assigning the same path twice is a caller bug and asserts: two code paths writing one property is a mistake
    /// worth surfacing rather than silently resolving, and an op may not carry a path twice in any case.
    op_builder& set_raw(property_path const& path, value v);
    op_builder& set_raw(entity_id entity, component_type_id component, property_id property, value v);

    /// Materializes the touched entities as seen from this op's parents, and emits only what actually differs.
    ///
    /// The diff has four cases, and the last is the one worth knowing:
    ///
    /// | current state of the path      | emit?                  |
    /// |--------------------------------|------------------------|
    /// | absent                         | yes                    |
    /// | one writer, bytes equal        | no                     |
    /// | one writer, bytes differ       | yes                    |
    /// | two or more writers            | yes, whatever the bytes|
    ///
    /// A multi-valued property is two independent writes rather than a value, so nothing about it equals the desired
    /// one — and emitting is the only way a conflict is ever resolved through the normal edit path.
    /// See [decisions.md](../../docs/decisions.md#a-multi-valued-property-always-differs).
    ///
    /// The result is not added to the graph; that is the caller's `graph.add(...)`.
    [[nodiscard]] op build(op_graph const& graph) const;

private:
    struct pending_write
    {
        property_path path;
        value v;
    };

    cc::vector<op_id> _parents;
    value _metadata;
    cc::vector<pending_write> _writes;
};
