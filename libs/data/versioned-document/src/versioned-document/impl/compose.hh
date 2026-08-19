#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <versioned-document/raw_document.hh>

/// Composing several layers' raw documents into one, entity by entity.
///
/// **The entity is the composition unit**, because it is what the selection phase takes: `impl::select_entity` is handed
/// one `raw_entity`, so composing at that granularity lets a layered parse and a layered apply reuse the ordinary
/// selection and construction code rather than growing a second copy of it.
///
/// The design is [layering](../../../docs/concepts/layering.md).

namespace vdoc::impl
{
/// One layer, as the composition sees it — the rest of what a layer is does not reach here.
struct layer_view
{
    raw_document const* document = nullptr;
};

/// The composed form of some set of entities, held for as long as a parse needs it.
///
/// **Composed entities are held all at once rather than one at a time, and that is a lifetime requirement rather than a
/// convenience.** A parse's selection phase records `raw_component const*` into what it was handed and its construction
/// phase reads them afterwards, so a single reused buffer would dangle every entity but the last.
///
/// It borrows every layer's bytes and owns none of them, so it carries `raw_document`'s lifetime rules across several
/// sources at once: valid only while every layer, and everything each layer borrows from, is alive and unmodified.
struct composed_document
{
    /// Sorted by entity id bytes.
    cc::vector<entity_id> entities;

    /// Parallel to `entities`. An entry can be empty where every layer's contribution was dropped.
    cc::vector<raw_entity> composed;

    /// The (entity, component) pairs dropped because their layers disagreed about `$schema_version`.
    /// `property` is empty: the finding is about the component, not about one path under it.
    cc::vector<property_path> schema_version_conflicts;

    /// The composed entity, or null where nothing was composed for it.
    [[nodiscard]] raw_entity const* try_get(entity_id entity) const;
};

/// Composes `entities` across `layers`, given bottom-first, with a higher layer replacing a lower one per PROPERTY PATH.
///
/// **Replacement is per path, and it is the whole point.** A higher layer holding `transform/position` must not shadow a
/// lower layer's `transform/rotation`, or an override would freeze every sibling property at the moment it was made.
/// Where a layer does win a path, its **entire** writer list replaces the lower one — so a multi-value inside that layer
/// survives and reaches the policy exactly as it would without layering, and conflicts stay layer-local.
///
/// `entities` must be sorted by id bytes; it is normally `compose_entities`, or the dirty subset of it.
///
/// **`$schema_version` composes per path like everything else, and that is a hazard rather than a feature**, so it is
/// checked here: every layer contributing a real property to a component and also stamping a version must agree with the
/// stamp that won, or the component is dropped and recorded in `schema_version_conflicts`.
/// Otherwise a layer overriding one property with an older shape would have the component read at the newer version and
/// misparse — or, worse, a too-new stamp would make the whole component vanish behind an `unknown_schema_version`.
/// A layer that does not stamp has **no opinion** rather than "version 0".
[[nodiscard]] composed_document compose(cc::span<layer_view const> layers, cc::span<entity_id const> entities);

/// Every entity any layer carries, sorted by id bytes and deduplicated.
[[nodiscard]] cc::vector<entity_id> compose_entities(cc::span<layer_view const> layers);
} // namespace vdoc::impl
