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
/// `shader` is an HLSL **fragment**, not a compilable shader.
/// It carries no bindings and, normally, no sampling code: it reads each signature attribute as an already-initialized local of
/// the type `attribute_format` maps to, and assigns the surface output.
/// Everything around it — the bindless declarations, the parameter block, one initializer per attribute — is generated per
/// permutation, which is what lets every material that samples no texture share one compiled shader.
///
/// `hash` is the content key over name, signature and shader together.
/// Two types built from equal inputs hash equal, so a library registering the same type twice keeps one.
struct sv::material_type
{
    /// the string id an `acquire(name)` looks up; unique within a library
    cc::string name;

    /// every attribute this type reads, in the order a generated shader declares them
    cc::vector<material_signature_entry> signature;

    cc::string shader;

    cc::hash128 hash;

    /// Hashes `name`, `signature` and `shader` into the content key.
    /// A signature declaring one name twice asserts: the resolver would have no way to say which declaration a binding meant.
    [[nodiscard]] static material_type create(cc::string name,
                                              cc::vector<material_signature_entry> signature,
                                              cc::string shader);

    /// The declaration of `name`, or null if this type does not read it.
    [[nodiscard]] material_signature_entry const* find(cc::string_view name) const;
};
