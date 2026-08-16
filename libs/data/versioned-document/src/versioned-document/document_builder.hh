#pragma once

#include <clean-core/function/function_ref.hh>
#include <versioned-document/change_summary.hh>
#include <versioned-document/document.hh>

/// Evolving a typed document without rebuilding it.
///
/// The design is [the typed document](../../docs/concepts/the-typed-document.md#evolving-a-document).

/// Evolves a document by **consuming** it.
///
/// A document you are holding is still immutable, and there is still no `set` on it.
/// What changed is that this exists: the only way from one document to the next without a full parse, and it takes
/// the old one away to get there.
///
/// That keeps the property immutability was bought for — a parsed document is safe to hand to another thread and hold
/// for as long as it is useful — while dropping the requirement that a one-entity edit rebuild the whole index.
/// It is the same shape as `op_builder` and `op`: a mutable staging type beside an immutable result.
///
///     auto b = vdoc::document_builder(cc::move(doc));
///     b.set_component(schema, entity, [&](byte* slot) { return schema.parse_into(slot, reader); });
///     doc = cc::move(b).freeze();
///
/// **This is a storage edit and knows nothing about interpretation.** It does not read `$alive`, consult a policy or
/// file a diagnostic; `vdoc::apply` is what does all three and drives this underneath.
class vdoc::document_builder
{
    // lifetime
public:
    /// Adopts `doc`, which is left empty.
    /// A default-constructed document is fine, and is how a builder starts from nothing.
    explicit document_builder(document&& doc);

    document_builder(document_builder&&) noexcept = default;
    document_builder& operator=(document_builder&&) noexcept = default;
    document_builder(document_builder const&) = delete;
    document_builder& operator=(document_builder const&) = delete;

    // entities
public:
    /// Inserts into the entity table; a no-op where already present.
    /// True where something was inserted.
    bool insert_entity(entity_id entity);

    /// Removes from the entity table, and **every component on it**.
    /// An entity that is gone cannot keep components, and leaving them would make `entities()` and the columns
    /// disagree about what exists.
    /// True where something was removed.
    bool remove_entity(entity_id entity);

    // components
public:
    /// Constructs one component into this entity's slot, creating the column where the type has none yet.
    ///
    /// An existing component is destroyed first, so `construct` always sees uninitialized storage — the same contract
    /// `component_schema::parse_into` has in a parse.
    /// `construct` returning false removes the component rather than leaving a half-built one, which is exactly what
    /// a parse dropping a component does.
    ///
    /// The entity must already be in the table; a component on an entity that does not exist is not a state the
    /// document can be in, and this asserts rather than inserting one behind the caller.
    ///
    /// Reports `removed` where `construct` declined and there was nothing there either.
    change_kind set_component(component_schema const& schema,
                              entity_id entity,
                              cc::function_ref<bool(byte* slot)> construct);

    /// True where a component was removed.
    bool remove_component(component_type_id type, entity_id entity);

    // queries
public:
    [[nodiscard]] bool contains_entity(entity_id entity) const;

    /// Whether this entity carries a component of that type, without knowing the C++ type it is.
    [[nodiscard]] bool has_component(component_type_id type, entity_id entity) const;

    /// Every component type with at least one instance, sorted by type id bytes.
    /// What an apply walks to find the components an entity used to have and no longer does.
    [[nodiscard]] cc::vector<component_type_id> component_types() const;

    // maintenance
public:
    /// Relocates everything into a fresh arena, reclaiming what in-place edits left behind.
    ///
    /// An arena is a bump allocator, so growing a column abandons its old arrays rather than freeing them.
    /// This is an order of magnitude cheaper than a re-parse — no value is decoded, only moved — and it is the
    /// caller's call rather than something `freeze` does, so the latency spike stays where it can be seen.
    void compact();

    [[nodiscard]] isize dead_arena_bytes() const;
    [[nodiscard]] isize live_arena_bytes() const;

    /// The immutable document again; the builder is empty afterwards.
    [[nodiscard]] document freeze() &&;

private:
    /// The index of the column for this schema, inserting an empty one in sorted position where absent.
    /// An index rather than a reference, because growing a column reallocates its arrays and reordering the column
    /// vector moves the columns themselves.
    [[nodiscard]] isize impl_column_for(component_schema const& schema);

    /// Makes room for one more component in `column`, growing both its arrays if it is at capacity.
    void impl_reserve_one(impl::component_column& column);

    /// Drops the column at `index` where it has no components left.
    void impl_drop_if_empty(isize index);

    document _doc;
};
