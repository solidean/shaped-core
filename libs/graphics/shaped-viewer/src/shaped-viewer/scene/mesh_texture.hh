#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <typed-geometry/linalg/vec.hh>

/// Where one component of a sampled attribute comes from: a channel of the texel, or a constant.
///
/// `zero` and `one` are here because a packed texture rarely lines up with the attribute reading it — an ORM map bound as a
/// 3-vector wants its own three channels, but a two-channel normal map bound as one wants a reconstructed third.
enum class sv::texture_channel : sv::u8
{
    r,
    g,
    b,
    a,
    zero,
    one,
};

/// Which channels of a sampled texel fill an attribute's components, in order.
///
/// Packed textures are the norm rather than the exception, and this is what lets one upload serve several attributes:
/// a metallic-roughness map binds twice (`base_metalness` from `.b`, `specular_roughness` from `.g`) and a base color map binds
/// twice (`base_color` from `.rgb`, `opacity` from `.a`), over a single resident texture, since the content hash is the same
/// bytes either way.
///
/// Only the first `component_count()` selectors of the declaration being filled are read; the rest are never looked at, which is
/// what makes an identity swizzle hash identically however its unread tail was spelled.
struct sv::channel_swizzle
{
    /// r, g, b, a — the identity, which is what an attribute reading a texture written for it wants
    texture_channel components[4] = {texture_channel::r, texture_channel::g, texture_channel::b, texture_channel::a};

    /// A swizzle over as many selectors as are given; the tail keeps the identity and is never read anyway.
    [[nodiscard]] static constexpr channel_swizzle of(texture_channel c0,
                                                      texture_channel c1 = texture_channel::g,
                                                      texture_channel c2 = texture_channel::b,
                                                      texture_channel c3 = texture_channel::a)
    {
        return {.components = {c0, c1, c2, c3}};
    }

    /// The one selector a scalar attribute reads — `of_channel(texture_channel::g)` is a roughness out of a packed map.
    [[nodiscard]] static constexpr channel_swizzle of_channel(texture_channel c) { return of(c); }

    /// Does this read the texel straight through, for an attribute of `component_count` components?
    /// Only the selectors that are read count, which is why the count has to be passed in.
    [[nodiscard]] constexpr bool is_identity(int component_count) const
    {
        for (auto i = 0; i < component_count; ++i)
            if (components[i] != texture_channel(i))
                return false;
        return true;
    }

    [[nodiscard]] friend constexpr bool operator==(channel_swizzle const&, channel_swizzle const&) = default;
};

/// An affine remap of a sampled attribute's components, applied AFTER the swizzle: `value = texel * scale + bias`.
///
/// It exists because a texture rarely stores what the attribute reading it means.
/// A tangent-space normal map stores `[0,1]` and means `[-1,1]`, which is a scale of 2 and a bias of -1 and nothing
/// more; glTF's `normalTexture.scale` folds into the same two numbers, and its `occlusionTexture.strength` is
/// `strength * texel + (1 - strength)`, which is again exactly this.
///
/// **The values live in the parameter block, not in the generated source.**
/// Only whether the transform is the identity reaches `permutation_key`, since that is what decides whether the shader
/// carries a multiply-add at all — so two materials differing only in a normal scale share one permutation, exactly as
/// gold and copper do.
/// That is the line the channel swizzle sits on the other side of: which channel is read is structure, what it is
/// scaled by is a value.
struct sv::sample_transform
{
    tg::vec4f scale = tg::vec4f(1, 1, 1, 1);
    tg::vec4f bias = tg::vec4f(0, 0, 0, 0);

    /// `[0,1]` read as `[-1,1]`, which is how every tangent-space normal map is stored.
    /// `xy_scale` is glTF's `normalTexture.scale` folded in: it scales the two tangential components and leaves the
    /// normal's own axis alone, because scaling that one would tilt nothing and only shorten the vector.
    [[nodiscard]] static constexpr sample_transform of_signed_normal(float xy_scale = 1.0f)
    {
        return {.scale = tg::vec4f(2 * xy_scale, 2 * xy_scale, 2, 2), .bias = tg::vec4f(-xy_scale, -xy_scale, -1, -1)};
    }

    /// `strength * texel + (1 - strength)`, which is glTF's `occlusionTexture.strength`: at 0 the map is ignored, at 1
    /// it is taken as written.
    [[nodiscard]] static constexpr sample_transform of_strength(float strength)
    {
        auto const rest = 1.0f - strength;
        return {.scale = tg::vec4f(strength, strength, strength, strength), .bias = tg::vec4f(rest, rest, rest, rest)};
    }

    /// Does this leave the first `component_count` components alone?
    /// Only the components an attribute reads count, exactly as with the swizzle — which is what lets an unread tail
    /// hold anything without forking a permutation.
    [[nodiscard]] constexpr bool is_identity(int component_count) const
    {
        for (auto i = 0; i < component_count; ++i)
            if (scale[i] != 1.0f || bias[i] != 0.0f)
                return false;
        return true;
    }

    [[nodiscard]] friend constexpr bool operator==(sample_transform const&, sample_transform const&) = default;
};

/// A texture read through a mesh's uv attribute — everything needed to sample one, minus what it is sampled *for*.
///
/// `uv_attribute` names a `mesh_attribute` on whatever mesh this ends up drawing, and is resolved per mesh rather than here:
/// one material draws on two meshes whose uv sets may be named differently, and neither knows about the other.
/// A mesh carrying no such attribute cannot sample, so a material attribute bound to this loses to the next-coarsest frequency —
/// the same fallback every other rank gets, not a special case.
///
/// This is the finest frequency an attribute can be bound at: it varies per pixel.
///
/// The `texture_id` is already minted, which makes this the GPU side of the pair: `texture_sample` is the same sample
/// described by its pixels instead.
struct sv::texture_sample_source
{
    texture_id texture = texture_id::invalid;
    cc::string uv_attribute = "uv";
    sg::sampler sampler = {};

    /// which channels of the texel fill the attribute's components; part of the permutation, since it is generated code
    channel_swizzle swizzle = {};

    /// what those components are scaled and shifted by afterwards; a value, so it lives in the parameter block
    sample_transform transform = {};
};

/// A texture the mesh offers its material, under the slot `name` identifies (`"albedo"`, `"roughness"`, ...).
///
/// The mesh names *which* texture and how to read it, the material names *what for* — the same contract the attributes travel under.
/// A slot whose name no attribute of the material's type declares is simply unused; a mesh carrying a texture nobody asked for is
/// not an error.
struct sv::mesh_texture_binding
{
    /// the material attribute this fills — matched against the type's signature by name
    cc::string name;

    texture_sample_source source;
};

/// The CPU counterpart of `texture_sample_source`: the same sample, carrying pixels rather than an already-minted id.
///
/// This is what removes the asymmetry an `sv::mesh` would otherwise have — its geometry and attributes travel as pinned
/// bytes while its textures would have needed a resource manager to exist before the mesh could be described at all.
/// `texture_data` is itself pinned and content-hashed, so minting the id is a lookup once a manager is at hand.
struct sv::texture_sample
{
    sv::texture_data texture;
    cc::string uv_attribute = "uv";
    sg::sampler sampler = {};

    channel_swizzle swizzle = {};
    sample_transform transform = {};
};

/// The CPU counterpart of `mesh_texture_binding` — the slot name plus the sample's pixels.
struct sv::mesh_texture
{
    /// the material attribute this fills — matched against the type's signature by name
    cc::string name;

    texture_sample source;
};
