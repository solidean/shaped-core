#pragma once

#include <clean-core/bytes/hash128.hh> // cc::hash128
#include <clean-core/common/assert.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/mesh_attribute.hh> // attribute_format / attribute_frequency, which a binding carries
#include <shaped-viewer/scene/mesh_flags.hh>
#include <shaped-viewer/scene/mesh_texture.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/transform/transform.hh>

/// One attribute of an `sv::resident_mesh`: the uploaded bytes, plus everything a material needs to match and read them.
///
/// It is the GPU counterpart of `sv::mesh_attribute`, and carries the same name, format and frequency because those are what the
/// resolve matches on — a binding whose format differs from a declaration's loses to the next-coarsest rank exactly as a CPU
/// attribute does.
///
/// `attribute` is the uploaded buffer, and is `invalid` at `per_instance`: one element for the whole mesh is a constant folded
/// into the parameter block, so it never becomes a resource at all and travels in `value` instead.
/// `hash` is the content key the parameter block is keyed by, kept here so a mesh whose bytes never reached the CPU still has an
/// identity to fold in.
struct sv::mesh_attribute_binding
{
    cc::string name;

    attribute_format format = attribute_format::of_scalar(scalar_type::f32);
    attribute_frequency frequency = attribute_frequency::per_vertex;

    /// the uploaded elements; `invalid` at `per_instance`, where `value` carries them instead
    attribute_id attribute = attribute_id::invalid;

    /// the single element of a `per_instance` attribute; empty at every other frequency
    cc::pinned_data<byte const> value;

    cc::hash128 hash;

    /// The binding for a CPU attribute already uploaded to `id`: its name, shape and content key come across unchanged.
    /// `id` must be `invalid` exactly when `a` is `per_instance`, whose one element travels as bytes instead of as a resource.
    [[nodiscard]] static mesh_attribute_binding of(mesh_attribute const& a, attribute_id id)
    {
        auto const per_instance = a.frequency == attribute_frequency::per_instance;
        CC_ASSERT((id == attribute_id::invalid) == per_instance, "a per_instance attribute is the one that uploads "
                                                                 "nothing");
        return {.name = a.name,
                .format = a.format,
                .frequency = a.frequency,
                .attribute = id,
                .value = per_instance ? a.data : cc::pinned_data<byte const>(),
                .hash = a.hash};
    }
};

/// One renderable mesh as resources: geometry and attributes already on the GPU, placed by a transform and drawn by a material.
///
/// This is the GPU half of the pair `sv::mesh` completes — see libs/graphics/shaped-viewer/docs/asset-loading.md.
/// A mesh cannot exist without a resource manager, which is exactly what makes it the form that admits geometry and attributes
/// that were never on the CPU at all: a compute pass produces a buffer, the manager adopts it, and nothing here asks for bytes.
/// `gpu_resource_manager::create_mesh` is what mints one; the ids it holds stay resident under the manager's own budget.
///
/// The lists mean what they mean on `mesh`: `attributes` and `textures` are what the material looks up by name, and a
/// material that misses one falls back rather than failing.
///
/// `bounds`, `triangle_count` and `vertex_count` are the CPU-side summary.
/// They are here because with GPU-only data nothing else can answer a camera-framing question, and because a placeholder drawn
/// while the real geometry is still arriving needs an extent to be drawn at.
/// `bounds` is empty when nothing declared one, which is the honest answer for an adopted buffer.
struct sv::resident_mesh
{
    /// human-readable, for debugging and for picking a mesh out of a scene; not an identity — nothing dedupes on it
    cc::string name;

    /// the uploaded geometry and the BLAS built from it
    mesh_id geometry = mesh_id::invalid;

    /// arbitrary extra data, looked up by name by the material — per element of the geometry, or one value for the whole mesh at `per_instance`
    cc::vector<mesh_attribute_binding> attributes;

    /// placement in world space; may scale or shear, so build it from tg's factories and `tg::compose`
    tg::affine_transform3f transform = {};

    /// how this mesh is rendered — the definition lives elsewhere and is shared
    material_id material = material_id::invalid;

    mesh_flags flags = mesh_flags_default;

    /// textures the material may bind, by slot name
    cc::vector<mesh_texture_binding> textures;

    /// the object-space extent, in the geometry's own frame — empty when nothing declared one
    cc::optional<tg::aabb3f> bounds;

    isize triangle_count = 0;
    isize vertex_count = 0;

    [[nodiscard]] bool is_visible() const { return flags.has(mesh_flag::visible); }
};
