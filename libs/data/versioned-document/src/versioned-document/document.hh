#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <versioned-document/component.hh>

#include <utility>

/// The typed document: an immutable index, built once by a parse and queried many times.
///
/// The design is [the concept](../../docs/concepts/the-typed-document.md).

namespace vdoc::impl
{
/// One component type's storage: two parallel dense arrays, both sorted by entity id bytes.
///
/// The components are type-erased bytes plus the schema's size and destroy entry point, which is what lets a document
/// hold component types it was never templated on.
/// A column owns nothing but arena bytes and points at no other column, which is what keeps structural sharing —
/// handing a re-parse an untouched column — reachable without changing a single query.
struct component_column
{
    component_schema schema;

    /// `count` entity ids, sorted by id bytes, parallel to `components`.
    /// Mutable because document_builder shifts them; every way out of a document hands out a const span.
    entity_id* entities = nullptr;

    /// `count` components of `schema.component_size` bytes each.
    byte* components = nullptr;

    isize count = 0;

    /// How many the two arrays were allocated for.
    /// A builder inserting past this reallocates both from the arena and leaves the old bytes behind as dead.
    isize capacity = 0;

    [[nodiscard]] cc::span<entity_id const> entity_ids() const { return cc::span<entity_id const>(entities, count); }

    /// The index of this entity's component, or -1 when it has none.
    [[nodiscard]] isize index_of(entity_id entity) const;
};
} // namespace vdoc::impl

/// The typed, immutable index an application queries.
///
/// **No mutation API, ever** — no set, no remove, no create.
/// That is not a limitation to be lifted later: edits go through an op and re-materialize, which is the only path that
/// keeps history honest, and immutability is what makes a document safe to hand to another thread and hold
/// indefinitely.
///
/// The way to the NEXT document without a full parse is [document_builder](document_builder.hh), which consumes one to
/// produce another.
/// A document you are holding still cannot change; getting a new one takes the old one away.
///
///     auto const doc = vdoc::parse(raw, policy, report);
///     if (auto const* const t = doc.get<my_transform>(vdoc::entity_id::of("wall-17")))
///         use(*t);
///     doc.each<my_transform, my_mesh>([](vdoc::entity_id e, my_transform const& t, my_mesh const& m) { ... });
///
/// A document owns copies of everything it keeps, so it outlives both the raw document and the op_graph behind it.
///
/// There is deliberately **no `operator==`**: comparing two documents would need per-type equality the library cannot
/// require of a component.
/// Compare the observable surface instead.
class vdoc::document
{
    // lifetime
public:
    document();
    ~document();

    document(document const&) = delete;
    document& operator=(document const&) = delete;

    document(document&& rhs) noexcept;
    document& operator=(document&& rhs) noexcept;

    // entity queries
public:
    /// Every instantiated entity, sorted by id bytes.
    /// An entity that survived with no known component is still here: it exists, and it is alive.
    [[nodiscard]] cc::span<entity_id const> entities() const
    {
        return cc::span<entity_id const>(_entities, _entity_count);
    }

    [[nodiscard]] isize entity_count() const { return _entity_count; }

    [[nodiscard]] bool contains(entity_id entity) const;

    /// Every component type with at least one instance, sorted by type id bytes.
    [[nodiscard]] cc::vector<component_type_id> component_types() const;

    [[nodiscard]] isize count_of(component_type_id type) const;

    // component queries
public:
    /// This entity's component, or null when it has none.
    /// A binary search over the type's entity id array.
    template <class ComponentT>
    [[nodiscard]] ComponentT const* get(entity_id entity) const
    {
        static_assert(is_component<ComponentT>, "specialize vdoc::component_traits<C> first - see "
                                                "docs/concepts/interpretation.md#components-belong-to-the-application");

        auto const* const column = try_find_column(impl::component_type_of<ComponentT>());
        if (column == nullptr)
            return nullptr;

        assert_type_matches<ComponentT>(*column);

        auto const i = column->index_of(entity);
        return i < 0 ? nullptr : component_at<ComponentT>(*column, i);
    }

    template <class ComponentT>
    [[nodiscard]] bool has(entity_id entity) const
    {
        return get<ComponentT>(entity) != nullptr;
    }

    /// Whether this entity carries a component of that type, without knowing the C++ type it is.
    /// The untyped counterpart of `has<C>`, for a caller walking `component_types()`.
    [[nodiscard]] bool has_component(component_type_id type, entity_id entity) const;

    /// Calls `f(entity_id, ComponentTs const&...)` for every entity carrying all of them, in ascending entity id bytes.
    ///
    /// One type is a linear scan of contiguous memory.
    /// Several is a leapfrog sorted-merge intersection over the entity id arrays — no sparse sets and no indirection.
    ///
    /// `f` is a deduced template parameter rather than a function_ref, so nothing indirect lands in the innermost loop
    /// of the one query this layout exists for.
    template <class... ComponentTs, class F>
    void each(F&& f) const
    {
        static_assert(sizeof...(ComponentTs) > 0, "each needs at least one component type");
        static_assert((is_component<ComponentTs> && ...),
                      "every type must specialize vdoc::component_traits - see "
                      "docs/concepts/interpretation.md#components-belong-to-the-application");

        constexpr auto n = isize(sizeof...(ComponentTs));
        impl::component_column const* columns[n] = {try_find_column(impl::component_type_of<ComponentTs>())...};

        auto column_index = isize(0);
        (assert_type_matches<ComponentTs>(columns[column_index++]), ...);

        for (auto const* const c : columns)
            if (c == nullptr || c->count == 0)
                return;

        isize cursors[n] = {};

        if constexpr (n == 1)
        {
            for (auto i = isize(0); i < columns[0]->count; ++i)
                (f(columns[0]->entities[i], *component_at<ComponentTs>(*columns[0], i)), ...);
            return;
        }
        else
        {
            // Invariant: `matched` columns have confirmed `key`, counting back from the one that raised it.
            auto key = columns[0]->entities[0];
            auto matched = isize(1);
            auto next = isize(1);

            while (true)
            {
                if (matched == n)
                {
                    invoke_at<ComponentTs...>(f, key, columns, cursors);

                    for (auto k = isize(0); k < n; ++k)
                        ++cursors[k];

                    if (cursors[0] == columns[0]->count)
                        return;

                    key = columns[0]->entities[cursors[0]];
                    matched = 1;
                    next = 1;
                    continue;
                }

                auto const& col = *columns[next];
                while (cursors[next] < col.count && col.entities[cursors[next]].compare_bytes(key) < 0)
                    ++cursors[next];

                if (cursors[next] == col.count)
                    return;

                if (col.entities[cursors[next]].compare_bytes(key) > 0)
                {
                    key = col.entities[cursors[next]];
                    matched = 1;
                }
                else
                    ++matched;

                next = (next + 1) % n;
            }
        }
    }

private:
    template <class ComponentT>
    [[nodiscard]] static ComponentT const* component_at(impl::component_column const& column, isize i)
    {
        return reinterpret_cast<ComponentT const*>(column.components) + i;
    }

    template <class ComponentT>
    static void assert_type_matches(impl::component_column const* column)
    {
        CC_ASSERT(column == nullptr || column->schema.type_key == impl::component_type_key<ComponentT>(),
                  "two C++ types are registered under one component type name");
    }

    template <class ComponentT>
    static void assert_type_matches(impl::component_column const& column)
    {
        assert_type_matches<ComponentT>(&column);
    }

    template <class... ComponentTs, class F>
    static void invoke_at(F& f, entity_id entity, impl::component_column const* const* columns, isize const* cursors)
    {
        [&]<cc::isize... I>(std::integer_sequence<cc::isize, I...>)
        {
            f(entity, *component_at<ComponentTs>(*columns[I], cursors[I])...);
        }(std::make_integer_sequence<cc::isize, isize(sizeof...(ComponentTs))>{});
    }

    [[nodiscard]] impl::component_column const* try_find_column(component_type_id type) const;

    /// Destroys every live component; the arena bytes themselves go with the arena.
    void destroy_components();

private:
    /// Declared first so it is released last — after every column's components have been destroyed in it.
    cc::unique_ptr<impl::document_arena> _arena;

    /// Sorted by entity id bytes, in the arena.
    entity_id* _entities = nullptr;
    isize _entity_count = 0;
    isize _entity_capacity = 0;

    /// Sorted by component type id bytes.
    cc::vector<impl::component_column> _columns;

    friend class impl::parser;
    friend class document_builder;
};

/// Fills a document, and the only thing that can.
///
/// The parser drives it, and a test may drive it directly to build a document without a raw document behind it.
/// Columns are added in ascending component type id order, and each column's entities in ascending entity id order;
/// both are the order a parse walks in anyway.
class vdoc::impl::parser
{
    // lifetime
public:
    parser();
    ~parser();

    parser(parser const&) = delete;
    parser& operator=(parser const&) = delete;

    // building
public:
    /// The entity table, which must already be sorted by entity id bytes.
    void set_entities(cc::span<entity_id const> entities);

    /// Opens a column with room for `capacity` components, which is an upper bound rather than a count.
    void begin_column(component_schema const& schema, isize capacity);

    /// Constructs one component into the open column, keeping it only if `construct` returns true.
    /// Returns what `construct` returned.
    bool push_component(entity_id entity, cc::function_ref<bool(byte* slot)> construct);

    /// Closes the open column, dropping it entirely when nothing was kept.
    void end_column();

    /// The finished document; the parser is empty afterwards.
    [[nodiscard]] document finish();

    /// Where a column's storage comes from, for a caller that needs the same lifetime.
    [[nodiscard]] document_arena& arena() const;

private:
    document _doc;

    /// -1 when no column is open.
    isize _open_column = -1;

    /// The open column's capacity, so pushing past it can assert.
    isize _open_capacity = 0;

    /// The open column's entity ids, which are written in place as components are kept.
    entity_id* _open_entities = nullptr;
};
