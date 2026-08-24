#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-viewer/fwd.hh>

/// A texture read through a mesh's uv attribute — everything needed to sample one, minus what it is sampled *for*.
///
/// `uv_attribute` names a `mesh_attribute` on whatever mesh this ends up drawing, and is resolved per mesh rather than here:
/// one material draws on two meshes whose uv sets may be named differently, and neither knows about the other.
/// A mesh carrying no such attribute cannot sample, so a material attribute bound to this loses to the next-coarsest frequency —
/// the same fallback every other rank gets, not a special case.
///
/// This is the finest frequency an attribute can be bound at: it varies per pixel.
struct sv::texture_sample_source
{
    texture_id texture = texture_id::invalid;
    cc::string uv_attribute = "uv";
    sg::sampler sampler = {};
};

/// A texture the mesh offers its material, under the slot `name` identifies (`"albedo"`, `"roughness"`, ...).
///
/// The mesh names *which* texture and how to read it, the material names *what for* — the same contract the attributes travel under.
/// A slot whose name no attribute of the material's type declares is simply unused; a mesh carrying a texture nobody asked for is
/// not an error.
struct sv::mesh_texture
{
    /// the material attribute this fills — matched against the type's signature by name
    cc::string name;

    texture_sample_source source;
};
