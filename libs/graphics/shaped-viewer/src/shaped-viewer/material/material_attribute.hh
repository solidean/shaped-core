#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <shaped-viewer/scene/mesh_texture.hh>

/// Where a material attribute's value came from, ordered from the coarsest variation to the finest.
///
/// The enumerator ORDER is the precedence: a value that varies more finely wins over one that varies more coarsely.
/// So a per-pixel texture beats a per-corner attribute, which beats a constant on the material, which beats the type's own default.
/// Comparing two frequencies is comparing the enumerators, and `resolve_material` does nothing cleverer than that.
///
/// It is HOW FINELY a value varies that ranks it, never who owns it: a material's texture outranks a mesh's attribute, and only
/// the two texture ranks are settled by ownership at all.
///
/// The three geometric mesh frequencies (`per_vertex` / `per_corner` / `per_triangle`) collapse into the single `mesh_attribute` rank on purpose.
/// They are not orderable against one another, and a mesh carrying one name at two of them is an authoring error rather than a precedence question.
///
/// The two texture ranks are spelled out separately rather than tie-broken.
/// Both vary per pixel, so ranking them needs a rule either way; saying outright that the mesh's wins leaves nothing hidden for a reader to discover.
enum class sv::material_frequency : sv::u8
{
    material_type,        ///< the declaration's own default
    material,             ///< a constant on the material
    mesh_instance,        ///< a `per_instance` mesh attribute — one value for the whole mesh
    mesh_attribute,       ///< a `per_vertex` / `per_corner` / `per_triangle` mesh attribute
    material_texture,     ///< uv-sampled, named by the material
    mesh_texture_binding, ///< uv-sampled, offered by the mesh
};

/// How an attribute's three corner values are blended into the one the hit reads.
///
/// A property of the DECLARATION rather than of the mesh: what a value means is the type's to say, and the mesh only supplies
/// numbers.
/// It picks the generated interpolation code, so it is part of the permutation — which it already is, through the type's hash.
enum class sv::attribute_interpolation : sv::u8
{
    /// the barycentric weighted sum, which is what every ordinary quantity wants
    linear,

    /// a unit quaternion, blended as a rotation: each corner aligned into the first's hemisphere, then summed and normalized
    ///
    /// The alignment is the whole point.
    /// `q` and `-q` are the same rotation, so two corners can describe neighbouring frames and still be antipodal as
    /// 4-vectors; summing those cancels toward zero and the normalized result belongs to neither.
    /// Requires a 4-component f32 format, which `material_type::create` checks.
    rotation,
};

/// What kind of thing an attribute is bound to.
enum class sv::material_source_kind : sv::u8
{
    constant,       ///< bytes, exactly as many as the declaration's format takes
    texture_sample, ///< a texture, uv-sampled through a named mesh attribute
};

namespace sv::impl
{
/// The bytes of one attribute value, in the layout `attribute_format` names.
template <class T>
[[nodiscard]] cc::vector<byte> attribute_value_bytes(T const& value)
{
    return cc::vector<byte>::create_copy_of(cc::span<T const>(&value, 1).as_bytes());
}
} // namespace sv::impl

/// One attribute a material type requires per pixel: what it is called, what it holds, and what it is worth when nothing supplies it.
///
/// The default is what makes a missing attribute a non-event.
/// Every signature entry resolves to something, so a mesh carrying none of the data a type asks for still draws rather than failing.
///
/// `is_final` pins the default against every finer frequency, which is how a type declares an attribute it derives itself and no caller may replace.
struct sv::material_signature_entry
{
    cc::string name;
    attribute_format format = attribute_format::of_scalar(scalar_type::f32);

    /// exactly `format.size_bytes()` bytes
    cc::vector<byte> default_value;

    bool is_final = false;

    /// how the three corners of a hit triangle are blended when a mesh attribute supplies this
    attribute_interpolation interpolation = attribute_interpolation::linear;

    /// The declaration of `name` defaulting to `value`, deducing `format` from its type.
    /// The type must be a scalar or a tg vector / matrix over one — whatever `attribute_format_of` names.
    template <class T>
    [[nodiscard]] static material_signature_entry of(cc::string name, T const& value, bool is_final = false)
    {
        return {.name = cc::move(name),
                .format = attribute_format_of<T>,
                .default_value = impl::attribute_value_bytes(value),
                .is_final = is_final};
    }

    /// The same, for a declaration whose value is a unit quaternion and blends as a rotation.
    /// `T` must be a 4-component f32 — a `tg::vec4f` or a `tg::quat_f` — which `material_type::create` rechecks.
    template <class T>
    [[nodiscard]] static material_signature_entry of_rotation(cc::string name, T const& value, bool is_final = false)
    {
        return {.name = cc::move(name),
                .format = attribute_format_of<T>,
                .default_value = impl::attribute_value_bytes(value),
                .is_final = is_final,
                .interpolation = attribute_interpolation::rotation};
    }
};

/// One attribute a material binds, overriding whatever coarser frequencies resolved to.
///
/// A binding names an attribute the type declares; one naming anything else is rejected when the material is registered, not silently ignored.
/// `is_final` stops every FINER frequency from overriding this value — a mesh's roughness texture we know to be bad is refused by
/// binding roughness `final` on the material.
/// A `final` texture binding refuses them unconditionally, including when the mesh carries no uv set for its own sample.
struct sv::material_attribute_binding
{
    cc::string name;
    material_source_kind kind = material_source_kind::constant;

    /// `kind == constant`: exactly `format.size_bytes()` bytes of the declaration this binds
    cc::vector<byte> constant;

    /// `kind == texture_sample`
    texture_sample_source sample;

    bool is_final = false;

    /// Binds `name` to the constant `value`, deducing its format from the type.
    /// The format is checked against the declaration when the material is registered.
    template <class T>
    [[nodiscard]] static material_attribute_binding of(cc::string name, T const& value, bool is_final = false)
    {
        return {.name = cc::move(name),
                .kind = material_source_kind::constant,
                .constant = impl::attribute_value_bytes(value),
                .is_final = is_final};
    }

    /// Binds `name` to a uv-sampled texture.
    [[nodiscard]] static material_attribute_binding of_texture(cc::string name,
                                                               texture_sample_source source,
                                                               bool is_final = false)
    {
        return {.name = cc::move(name),
                .kind = material_source_kind::texture_sample,
                .sample = cc::move(source),
                .is_final = is_final};
    }

    /// The format a constant binding carries, which only its declaration can say — so this is what the check compares against.
    [[nodiscard]] bool fits(attribute_format format) const
    {
        return kind != material_source_kind::constant || constant.size() == format.size_bytes();
    }
};
