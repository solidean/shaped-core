#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/material_attribute.hh>

/// What `material_type::create` builds a type from, each field meaning what `material_type`'s field of the same name does.
/// Named field by field, so two attributes of one type cannot trade places unseen: an opacity cutoff and an emission scale are both floats.
struct sv::material_type_description
{
    cc::string name = {};
    cc::vector<material_signature_entry> signature = {};
    cc::string shader = {};
    cc::string opacity_attribute = {};
    cc::string opacity_cutoff_attribute = {};
    cc::string occlusion_attribute = {};
    cc::string emission_color_attribute = {};
    cc::string emission_scale_attribute = {};
    cc::string base_color_attribute = {};
    cc::string roughness_attribute = {};
    cc::string metalness_attribute = {};
    cc::string specular_weight_attribute = {};
    cc::string transmission_attribute = {};
    cc::string thin_walled_attribute = {};
    cc::string tangent_frame_attribute = {};
    bool display_only = false;
    cc::string height_attribute = {};
    cc::string height_top_attribute = {};
    cc::string height_bottom_attribute = {};
};

/// A family of materials: what a shader needs per pixel, and the code that turns it into a shaded surface.
///
/// PBR is a material type; gold is a `sv::material` instantiating it.
/// The type is what declares the contract — a mesh's attributes and textures mean nothing until a type names them — and the
/// material is what fills it in.
///
/// `shader` is an SGL **fragment**, not a compilable shader.
/// It carries no bindings and, normally, no sampling code: it reads each signature attribute as an already-initialized,
/// immutable local of the type `attribute_format` maps to, and assigns the fields of `surface`, the one `let mut` there.
/// Its statements are written unindented, and the generator indents them into the material function's body.
/// Everything around it — the bindless loads, the parameter block, one local per attribute — is generated per permutation,
/// which is what lets every material that samples no texture share one compiled hit group.
///
/// A minimal type, which reads one color and is lit like plaster:
///
///     material_type::create({.name = "plaster",
///                            .signature = {material_signature_entry::of("tint", tg::vec3f(0.9f, 0.9f, 0.85f))},
///                            .shader = "surface.base_color = saturate(tint)\n"
///                                      "surface.specular_roughness = 0.8\n"});
///
/// `hash` is the content key over name, signature, fragment and the opacity, cutoff, occlusion and emission attributes together.
/// Two types built from equal inputs hash equal, so a library registering the same type twice keeps one.
struct sv::material_type
{
    /// the string id an `acquire(name)` looks up; unique within a library
    cc::string name;

    /// every attribute this type reads, in the order a generated shader declares them
    cc::vector<material_signature_entry> signature;

    /// the SGL fragment, run inside the generated material function
    cc::string shader;

    /// Which declared float attribute is this type's cutout opacity, a coverage fraction, or empty for a type that cannot cut out at all.
    ///
    /// **The fragment never computes it**: one generic reader takes it straight from the attribute, so a kernel that traces
    /// compiles once whatever materials a scene holds, and a fragment writing `surface.geometry_opacity` asserts.
    /// A permutation whose opacity attribute came through as the signature's own default cannot cut out; see `material_opacity`.
    cc::string opacity_attribute;

    /// Which declared float attribute thresholds the opacity, or empty for pure coverage.
    /// Above 0 a hit is kept where the opacity reaches it — glTF's MASK — and at 0 the opacity stays a coverage fraction.
    /// It must resolve to a value per instance; one sourced from a texture or a mesh attribute is ignored with a warning.
    cc::string opacity_cutoff_attribute;

    /// Which declared float attribute is baked ambient occlusion, or empty for none.
    ///
    /// The raster path multiplies its environment light by it, and nothing else: an analytic light's direct term is not occluded by it.
    /// The tracer never reads it, since it computes that occlusion itself and a baked term on top would count it twice.
    cc::string occlusion_attribute;

    /// Which declared float3 attribute is this type's emission color, or empty for a type that lights nothing.
    ///
    /// **Only a type declaring it lights the scene**: an instance of it may be sampled as a light.
    /// Its emitted radiance is `max(0, scale) * max(0, color)`, which the generator writes into `surface` after the fragment.
    /// The fragment of such a type may not write `surface.emission_*` itself, so the closest hit and the light sampler read one emission.
    /// A type that leaves it empty may still write emission, which then reaches the camera and nothing else.
    cc::string emission_color_attribute;

    /// Which declared float attribute scales the emission color, or empty for a scale of 1.
    cc::string emission_scale_attribute;

    /// Which declared float3 attribute is this type's base color, or empty for none, which a secondary hit reads as mid-gray.
    /// The hybrid lights a secondary hit as a diffuse surface of this color, read by one reader for every material rather than by its code.
    cc::string base_color_attribute;

    /// Which declared float attributes are this type's specular roughness and metalness, or empty for none.
    /// A secondary hit reads them beside the base color; one without a roughness stays matte, and one without a metalness is a dielectric.
    cc::string roughness_attribute;
    cc::string metalness_attribute;

    /// Which declared float attribute weighs this type's dielectric specular layer, or empty where it always has one.
    /// A secondary hit lights the layer at this weight, so a matte material does not gain a specular coat past the first bounce.
    cc::string specular_weight_attribute;

    /// Which declared float attributes are this type's transmission weight and its thin-wall switch, or empty for none.
    /// An instance with both, the weight above 0 and the switch on, is a pane the hybrid's rays pass through.
    cc::string transmission_attribute;
    cc::string thin_walled_attribute;

    /// Which declared rotation attribute is this type's tangent frame, or empty for none.
    /// The hybrid reads its +z as the smooth normal where a bounce meets the surface, so glass refracts and a secondary is lit without facets.
    cc::string tangent_frame_attribute;

    /// Whether this type is a display material, which reaches the camera and reflects nothing.
    /// A secondary hit then reads its base color and specular weight as 0, so the hybrid's bounce is absorbed there as a tracer's is,
    /// rather than lit as the mid-gray a type naming no base color is otherwise read as.
    bool display_only = false;

    /// Which declared float attribute is a relief's height in meters, positive out of the face, or empty for none.
    /// Bound to a texture, a hit marches it along its ray (parallax occlusion mapping) and every texture is read where the ray met it.
    /// The face's geometry stands at `height_top_attribute` and the relief is carved down to `height_bottom_attribute`;
    /// a top not above the bottom marches nothing.
    cc::string height_attribute;
    cc::string height_top_attribute;
    cc::string height_bottom_attribute;

    cc::hash128 hash;

    /// Hashes `name`, `signature`, `shader`, the opacity, cutoff and occlusion attributes and both emission attributes into the content key.
    /// A signature declaring one name twice asserts: the resolver would have no way to say which declaration a binding meant.
    /// So does an opacity, cutoff or occlusion attribute naming something the signature does not declare as a float.
    /// So do an emission color that is not a declared float3, and a fragment of an emitting type that writes `surface.emission_*`.
    [[nodiscard]] static material_type create(material_type_description description);

    /// Whether this type declares where its emission comes from, which is what lets an instance of it light the scene.
    [[nodiscard]] bool declares_emission() const { return !emission_color_attribute.empty(); }

    /// The declaration of `name`, or null if this type does not read it.
    [[nodiscard]] material_signature_entry const* find(cc::string_view name) const;
};
