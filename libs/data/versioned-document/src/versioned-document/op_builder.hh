#pragma once

#include <clean-core/container/set.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <versioned-document/component.hh>
#include <versioned-document/op.hh>
#include <versioned-document/op_graph.hh>
#include <versioned-document/snapshot_cache.hh>

/// Builds one op from a set of edits, diffing against its own parents so only changed properties are written.
///
/// **Pure: the builder holds no graph state of its own.**
/// It is a staging area for edits, and the graph only enters at build(), which is where the diff happens.
///
/// This is what keeps history small enough to keep forever, and it is why an op's assignment list is a genuine
/// changelog rather than a snapshot.
///
/// The design is [the concept](../../docs/concepts/ops-and-content-addressing.md#ops-write-only-what-changed).
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

    /// Stages a whole component: stamps `$schema_version`, then lets `component_traits<C>::write` emit the rest.
    ///
    /// Stamping happens here rather than in write, so there is one stamp site and a component author cannot forget it
    /// — component_writer rejects the sigil outright.
    /// The diff underneath is set_raw's, so an unchanged component emits no assignments at all, stamp included.
    template <class ComponentT>
    op_builder& set(entity_id entity, ComponentT const& c)
    {
        static_assert(is_component<ComponentT>, "specialize vdoc::component_traits<C> first - see "
                                                "docs/concepts/interpretation.md#components-belong-to-the-application");

        auto const type = impl::component_type_of<ComponentT>();
        set_raw(entity, type, reserved::schema_version(), value::of(component_traits<ComponentT>::schema_version));

        auto writer = component_writer::create_for(*this, entity, type);
        component_traits<ComponentT>::write(c, writer);
        return *this;
    }

    /// Deletion, which is an ordinary assignment and never a removal.
    ///
    /// Something is dead only if `$alive` is unambiguously false, so writing true here is how an undelete is spelled.
    op_builder& set_alive(entity_id entity, component_type_id component, bool alive);

    /// Entity-level deletion: `$alive` on the `$entity` component type, which drops the whole entity.
    op_builder& set_entity_alive(entity_id entity, bool alive);

    /// Shorthands for the two set_alive calls, so a call site says what it means rather than passing a bool.
    ///
    /// Restoring only means anything against a stored false — `$alive` absent already means alive, so restoring
    /// something that was never removed writes a property that says exactly the default.
    op_builder& remove_component(entity_id entity, component_type_id component)
    {
        return set_alive(entity, component, false);
    }
    op_builder& restore_component(entity_id entity, component_type_id component)
    {
        return set_alive(entity, component, true);
    }
    op_builder& remove_entity(entity_id entity) { return set_entity_alive(entity, false); }
    op_builder& restore_entity(entity_id entity) { return set_entity_alive(entity, true); }

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

    /// The same diff, with the parent materialization allowed to terminate at a cached snapshot.
    ///
    /// **The result is defined to equal the overload above, always.**
    /// A cache changes how long the diff takes and never what it produces, so the two are checked against each other
    /// by op id, which is total.
    ///
    /// The entity filter applies to assignments and never to edges, so it does not shorten the *walk* — a snapshot is
    /// the only thing that does, and this is how the edit path reaches one.
    /// Without it, every build replays the whole history to diff a single entity.
    [[nodiscard]] op build(op_graph const& graph, snapshot_cache& cache) const;

private:
    [[nodiscard]] op impl_build(op_graph const& graph, snapshot_cache* cache) const;

    struct pending_write
    {
        property_path path;
        value v;
    };

    cc::vector<op_id> _parents;
    value _metadata;
    cc::vector<pending_write> _writes;

    /// The duplicate-path check's index, built lazily once a linear scan per write stops being cheap.
    /// Populated only where CC_ASSERT is on, and empty otherwise.
    cc::set<property_path> _staged_paths;
};
