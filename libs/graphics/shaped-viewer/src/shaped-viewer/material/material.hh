#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/material_attribute.hh>

/// One concrete material: a material type with some of its attributes bound.
///
/// Gold is a `material` over the PBR `material_type` — one set of PBR values that mean gold.
/// It binds only what it cares about; every attribute it leaves alone falls through to the type's default, or to whatever the mesh
/// turns out to supply.
///
/// `hash` is the content key over the type id and the bindings.
/// It is keyed on the id rather than the type's own hash because a material only means anything inside the library that minted
/// that id, and that is the only place the key is used.
struct sv::material
{
    /// human-readable, for debugging and for `material_library::acquire(name)`
    cc::string name;

    material_type_id type = material_type_id::invalid;

    /// what this material overrides; each must name an attribute the type declares
    cc::vector<material_attribute_binding> overrides;

    cc::hash128 hash;

    /// Hashes `type` and `overrides` into the content key.
    /// Nothing is validated against the type here — `material` does not know its library — so a binding naming an attribute the
    /// type never declared is caught by `material_library::acquire`, which does.
    [[nodiscard]] static material create(cc::string name,
                                         material_type_id type,
                                         cc::vector<material_attribute_binding> overrides);

    /// The binding for `name`, or null if this material leaves it alone.
    [[nodiscard]] material_attribute_binding const* find(cc::string_view name) const;
};
