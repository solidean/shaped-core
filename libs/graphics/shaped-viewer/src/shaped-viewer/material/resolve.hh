#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/material_attribute.hh>

/// One attribute of a material type, resolved against a concrete material and mesh.
///
/// Exactly one of the three payloads is live, and `frequency` says which:
/// `constant` for the three constant ranks, `attribute` for `mesh_attribute`, `sample` for the two texture ranks.
/// (`mesh_instance` is a constant too — a `per_instance` mesh attribute is one value for the whole mesh, so it reads as bytes
/// like the other two rather than as an indexed load.)
///
/// Every payload BORROWS: the constant points into the declaration or the binding, `attribute` and `sample` into the mesh or the material.
/// A resolved material is therefore only valid while the library, the material and the mesh it was resolved from are.
struct sv::resolved_attribute
{
    /// the declaration's name, borrowed from it
    cc::string_view name;

    attribute_format format = attribute_format::of_scalar(scalar_type::f32);

    /// which rank won
    material_frequency frequency = material_frequency::material_type;

    /// the declaration's own, copied here because the generator sees the resolution rather than the signature
    /// It only changes anything when `frequency == mesh_attribute`; every other rank reads one value and blends nothing.
    attribute_interpolation interpolation = attribute_interpolation::linear;

    /// live when `frequency <= mesh_instance`
    cc::span<byte const> constant;

    /// live when `frequency == mesh_attribute`; its own `frequency` is the geometric one a shader indexes by
    mesh_attribute_binding const* attribute = nullptr;

    /// live when `frequency` is `material_texture` or `mesh_texture_binding`
    texture_sample_source const* sample = nullptr;

    /// The best CONSTANT the walk had before a sample won — live alongside `sample`, and never empty beside one.
    ///
    /// It is what a placeholder is seeded from while the texture is still arriving: a material whose base color map
    /// has not landed shows its own base color factor rather than black, which is the difference between an asset
    /// appearing dark and an asset appearing unlit.
    /// The declaration's default at worst, since every declaration has one.
    cc::span<byte const> fallback_constant;

    /// the mesh attribute `sample->uv_attribute` resolved to, live alongside `sample`
    ///
    /// A sample is only a candidate at all when the mesh carries this, so it is never null when `sample` is not.
    /// Its own frequency is what decides the generated load, which is why it is part of the permutation rather than a value.
    mesh_attribute_binding const* uv = nullptr;
};

/// A material type, a material and a mesh, resolved into one value per declared attribute — what a shader is generated from and
/// what a per-instance parameter slot is filled from.
///
/// **The two keys are the point of this type**, and they are deliberately not one.
///
/// `permutation_key` covers only the SHAPE of the resolution: which attribute won at which frequency, the geometric frequency of
/// a mesh-sourced one, the uv attribute and sampler of a sampled one, and the type's own hash (which carries the shader text).
/// It does NOT cover any resolved value.
/// So gold and copper — two materials over one type, differing only in their constants — share one generated shader and one
/// pipeline, and only a texture sample forces a second.
///
/// `parameter_key` covers the resolved values: the constant bytes, the texture ids, and the content hash of each mesh-sourced
/// attribute (which is what decides the buffer a slot points at).
/// It keys the per-instance slot those values are written into.
struct sv::resolved_material
{
    material_type const* type = nullptr;
    material const* source = nullptr;

    /// one entry per signature declaration, in signature order
    cc::vector<resolved_attribute> attributes;

    cc::hash128 permutation_key;
    cc::hash128 parameter_key;
};

namespace sv
{
/// Resolves every attribute `type` declares down the frequency chain, coarsest first, finest wins.
///
/// The walk is: the declaration's default, a constant on the material, a `per_instance` mesh attribute, a geometric mesh
/// attribute, a texture the material named, a texture the mesh offers.
/// Whichever rank supplied a value last is the winner, so the order of `material_frequency` is the whole rule.
///
/// A candidate that cannot be used is skipped rather than being an error, and skipping is what "a material falls back rather than
/// failing" means concretely:
/// a mesh attribute whose format differs from the declaration's, and a texture whose uv attribute the mesh does not carry, both
/// simply lose to the next-coarsest rank.
/// Every declaration therefore resolves to something, and a mesh carrying none of what a type asks for still draws.
///
/// `is_final` on the declaration or on a binding stops the walk there, so no FINER frequency overrides it.
/// That is how a material refuses a mesh's roughness texture: bind roughness final, and the texture never gets its turn.
/// It stops the walk even when its own candidate was skipped — a `final` texture binding whose uv set the mesh lacks leaves the
/// coarser winner standing rather than falling through to the mesh's texture.
///
/// `material.type` is not checked against `type` — the library is what pairs them, and it validates once at registration rather
/// than on every resolve.
[[nodiscard]] resolved_material resolve_material(material_type const& type,
                                                 material const& material,
                                                 sv::resident_mesh const& mesh);

/// The same, resolving `id` through `lib` — the form a renderer calls.
/// `id` must be one `lib` minted.
[[nodiscard]] resolved_material resolve_material(material_library const& lib,
                                                 material_id id,
                                                 sv::resident_mesh const& mesh);
} // namespace sv
