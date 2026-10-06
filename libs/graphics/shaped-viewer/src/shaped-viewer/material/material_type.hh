#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/material_attribute.hh>

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
///     material_type::create("plaster", {material_signature_entry::of("tint", tg::vec3f(0.9f, 0.9f, 0.85f))},
///                           "surface.base_color = saturate(tint)\n"
///                           "surface.specular_roughness = 0.8\n");
///
/// `hash` is the content key over name, signature, fragment and opacity attribute together.
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

    cc::hash128 hash;

    /// Hashes `name`, `signature`, `shader` and both opacity attributes into the content key.
    /// A signature declaring one name twice asserts: the resolver would have no way to say which declaration a binding meant.
    /// So does an opacity attribute naming something the signature does not declare as a float.
    [[nodiscard]] static material_type create(cc::string name,
                                              cc::vector<material_signature_entry> signature,
                                              cc::string shader,
                                              cc::string opacity_attribute = {},
                                              cc::string opacity_cutoff_attribute = {});

    /// The declaration of `name`, or null if this type does not read it.
    [[nodiscard]] material_signature_entry const* find(cc::string_view name) const;
};
