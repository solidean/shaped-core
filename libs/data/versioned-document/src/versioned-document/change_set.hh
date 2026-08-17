#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <versioned-document/ids.hh>

/// A dirty property path set: what has to be re-interpreted, and nothing about what it became.
///
/// This is the **input** side of an incremental re-interpretation, where `change_summary` is the output side.
/// An op chain produces one, a directly written source produces one, and layering unions the ones its layers produce
/// — so a re-interpretation no longer has to be phrased as "from this op to that op".
///
/// The design is [interpretation](../../docs/concepts/interpretation.md#applying-an-op-incrementally).

/// How much of each entry's path carries information.
///
/// Coarser is a superset: an entity-granular set says "something under this entity changed" and declines to say what.
enum class vdoc::change_granularity : vdoc::u8
{
    property,
    component,
    entity,
};

/// The paths to re-interpret, sorted and deduplicated under the prefix its granularity names.
///
/// **`covers` may answer true where nothing changed, and never false where something did.**
/// That is the whole contract, and it is what makes coarsening a pure speed dial: every consumer is correct at every
/// granularity, and only the amount of recomputation varies.
/// So a producer that cannot be precise is free to be coarse, and never has to be wrong.
///
/// Below the granularity, an entry's fields are **default ids that must not be read**.
/// An id has no invalid state — a default-constructed one equals `of("")`, which is a legal id that sorts first — so
/// the granularity, never the data, is what says how much of a path means anything.
/// Reading `entries()` directly is reading past that, which is why every question a consumer actually has is a method
/// here instead.
class vdoc::change_set
{
    // lifetime
public:
    change_set() = default;

    /// No honest delta is available, so every consumer must assume all of it changed.
    [[nodiscard]] static change_set everything();

    // queries
public:
    [[nodiscard]] change_granularity granularity() const { return _granularity; }

    /// True when nothing changed.
    /// `everything()` is never empty, and an empty set is never `everything()`.
    [[nodiscard]] bool is_empty() const { return _paths.empty() && !_everything; }

    [[nodiscard]] bool is_everything() const { return _everything; }

    /// Whether this path has to be re-interpreted, judged at whatever granularity the set carries.
    [[nodiscard]] bool covers(property_path const& path) const;

    /// Whether anything under this entity has to be re-interpreted.
    /// Answers in one search at every granularity, because a blanked field sorts before every real one.
    [[nodiscard]] bool covers_entity(entity_id entity) const;

    /// Every entity named, sorted by id bytes — what an apply's touched set wants.
    /// Asserts on `everything()`, which names no entities and means all of them.
    [[nodiscard]] cc::vector<entity_id> entities() const;

    /// The raw entries, for a consumer that has already read `granularity()` and knows how much to look at.
    [[nodiscard]] cc::span<property_path const> entries() const { return _paths; }

    // composition
public:
    /// Widens to a coarser granularity, in one forward pass — the entries are sorted entity-major, so everything that
    /// collapses together is already adjacent.
    ///
    /// **Asserts on a request to refine.** The information is not there, and inventing it would turn a conservative set
    /// into a lying one, which no consumer could detect.
    void coarsen_to(change_granularity granularity);

    /// Unions in another set, coarsening both to the coarser of the two granularities.
    /// `everything()` on either side absorbs.
    void union_with(change_set const& rhs);

    void clear();

private:
    /// Blanks every entry below `_granularity` and drops the duplicates that creates.
    /// `_paths` must already be sorted; blanking a suffix of a key cannot reorder, only collide.
    void impl_normalize();

private:
    /// Sorted by `property_path::compare_bytes`, deduplicated, and blanked below `_granularity`.
    /// Empty whenever `_everything` is set, which is the canonical form `is_empty` rests on.
    cc::vector<property_path> _paths;

    change_granularity _granularity = change_granularity::property;

    bool _everything = false;

    friend class change_set_builder;
};

/// Collects paths in any order, then establishes the invariant once.
///
/// Separate from `change_set` because a producer adds as it goes — a sorted insert per write would make writing a whole
/// source quadratic, and the sort is wanted once at the end rather than n times on the way.
/// Same shape as `op_builder` beside `op`.
class vdoc::change_set_builder
{
    // lifetime
public:
    /// The granularity the finished set carries.
    /// Blanking happens once, in `build`, rather than on every add.
    explicit change_set_builder(change_granularity granularity = change_granularity::property);

    // building
public:
    /// Records this path, in any order, and duplicates are free.
    void add(property_path const& path);

    /// Records everything under an entity.
    /// **This coarsens the whole set to entity granularity**, because a set carries one granularity and an
    /// entity-wide claim cannot be represented at a finer one.
    void add_entity(entity_id entity);

    /// Gives up on being precise, which every later `add` then costs nothing.
    void add_everything();

    [[nodiscard]] bool is_empty() const { return _paths.empty() && !_everything; }

    void clear();

    /// Sorts, deduplicates, and hands over the finished set.
    /// The builder is empty afterwards.
    [[nodiscard]] change_set build() &&;

private:
    cc::vector<property_path> _paths;
    change_granularity _granularity;
    bool _everything = false;
};
