#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>

#include <memory>

/// Which shader entry point a texture shape reads through, and the only thing its dimension decides.
///
/// This is the routine's PARAMETER, so it is public: the registry holds one box_filter_mipmap_routine instance per
/// variant, and a caller naming the type has to be able to name the parameter too.
enum class sr::mipmap_variant : sg::u8
{
    tex_1d,
    tex_1d_array,
    tex_2d,
    tex_2d_array, ///< also every cube and cube array, whose UAV is a 2D array of faces
    tex_3d,

    count_
};

/// One mipmap variant's compiled program: the group layout its bindings define, and the pipeline over it.
///
/// The two travel together because both fall out of the same compiled shader, and a caller needs the layout to
/// build its binding group — so caching only the pipeline would mean waiting on that shader a second time.
struct sr::mipmap_program
{
    sg::binding_group_layout_handle layout;
    sg::compute_pipeline_handle pipeline;
};

/// Fills a texture's mip chain by averaging each level into the next — a plain box filter.
///
/// Named for its filter on purpose.
/// A box filter is the cheap, separable default that suits "we uploaded the base level and want the rest", and
/// it is not the right answer everywhere: it ignores gamma, so averaging sRGB content darkens it, and it
/// aliases where a Kaiser or Mitchell filter would not.
/// Those belong in routines of their own rather than behind a flag here, and the name leaves room for them.
///
/// **Every mippable shape works** — 1D, 2D and 3D, arrays and cubes alike — because a mip chain is not a 2D
/// idea.
/// HLSL cannot abstract over view dimensions, so there is one shader entry point per dimension and `execute`
/// picks between them from the texture's own type; a shape that cannot carry mips at all fails to compile.
/// An array averages within a slice and never across slices, and a cube face is a slice, so nothing bleeds
/// between faces.
/// A 3D texture is the one shape that halves in z too, which is why it averages 8 taps rather than 4.
///
/// One dispatch per generated level, source bound as a single-mip view of level N and target as the UAV of
/// N+1, so no level is read and written by the same dispatch.
/// The texture must carry `readonly_texture | readwrite_texture` usage and have the levels allocated already —
/// this fills a chain, it never reshapes one.
class sr::box_filter_mipmap_routine : public sg::render_routine<box_filter_mipmap_routine, sr::mipmap_variant>
{
public:
    /// Generates levels `first_level` through the end of `texture`'s chain from the level below each.
    ///
    /// `first_level` must be >= 1 (level 0 is the source of everything and is never generated) and within the
    /// chain; generating from a level whose own contents are not yet uploaded produces garbage, so the caller
    /// orders this after the upload it depends on.
    /// Declines while this variant's shader is still compiling, and after a compile that failed.
    /// A texture with no level to generate is not a refusal — level_count says so first.
    template <class Traits>
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd,
                                                     sg::texture<Traits> const& texture,
                                                     int first_level = 1)
    {
        static_assert(!Traits::is_multisampled, "a multisampled texture has no mip chain to fill");

        if (level_count(texture, first_level) == 0)
            return sg::routine_outcome::executed;

        // Acquired ONCE for the whole chain rather than once per level: the variant is fixed by the texture's shape,
        // so every level of one call lands on the same instance.
        auto const self = try_acquire(cmd, _variant_of<Traits>());
        if (!self.is_ready() || self->_program == nullptr)
            return sg::routine_outcome::declined;

        auto const levels = texture.mip_levels();
        for (auto level = first_level; level < levels; ++level)
        {
            auto const e = _extent_of(texture, level);
            _dispatch_level(cmd, *self, _source_of(texture, level - 1), _target_of(texture, level), e.x, e.y, e.z);
        }
        return sg::routine_outcome::executed;
    }

    /// How many dispatches `execute` would record — what a caller budgeting GPU work per frame needs to know
    /// before it commits to the call.
    template <class Traits>
    [[nodiscard]] static int level_count(sg::texture<Traits> const& texture, int first_level = 1)
    {
        CC_ASSERT(first_level >= 1, "level 0 is the source of the chain and is never generated");
        auto const levels = texture.mip_levels();
        return first_level >= levels ? 0 : levels - first_level;
    }

private:
    template <class Traits>
    [[nodiscard]] static constexpr mipmap_variant _variant_of()
    {
        if constexpr (Traits::dimension == sg::texture_dimension::d1)
            return Traits::is_array ? mipmap_variant::tex_1d_array : mipmap_variant::tex_1d;
        else if constexpr (Traits::dimension == sg::texture_dimension::d3)
            return mipmap_variant::tex_3d;
        else
            return (Traits::is_array || Traits::is_cube) ? mipmap_variant::tex_2d_array : mipmap_variant::tex_2d;
    }

    /// The single-mip source view of `level`, in the dimension the matching entry point expects.
    ///
    /// A cube's natural SRV is a `TextureCube`, which is sampled by direction rather than indexed by texel — so
    /// a cube reads through its faces as a 2D array instead, matching the shape its UAV already has.
    template <class Traits>
    [[nodiscard]] static sg::raw_view _source_of(sg::texture<Traits> const& texture, int level)
    {
        if constexpr (Traits::is_cube)
            return texture.as_readonly_2d_array_view({.mips = {.start = level, .count = 1}});
        else
            return texture.as_readonly_view({.mips = {.start = level, .count = 1}});
    }

    /// The target (UAV) view of `level`.
    ///
    /// A 3D texture has to name its depth range explicitly: the default "all slices" is resolved against the
    /// texture's base depth, not the level's, so mip 1 of an 8-deep texture would ask for 8 slices of a 4-deep
    /// level and the view creation fails.
    /// Every other shape's slice count is the same at every level, so the default is right for them.
    template <class Traits>
    [[nodiscard]] static sg::raw_view _target_of(sg::texture<Traits> const& texture, int level)
    {
        if constexpr (Traits::dimension == sg::texture_dimension::d3)
            return texture.as_readwrite_view(
                {.mip = level, .depth_slices = {.start = 0, .count = _mip_extent(texture.depth(), level)}});
        else
            return texture.as_readwrite_view({.mip = level});
    }

    /// One axis of `level`, never below 1.
    [[nodiscard]] static constexpr int _mip_extent(int base, int level)
    {
        auto const e = base >> level;
        return e < 1 ? 1 : e;
    }

    /// The thread extent of one level, on each axis independently.
    struct extent
    {
        int x = 1;
        int y = 1;
        int z = 1;
    };

    /// How many slices the shape's array view carries, a cube's six faces per layer included.
    /// The same rule `texture::_whole_slice_count()` applies, so the dispatch covers exactly what the UAV does.
    template <class Traits>
    [[nodiscard]] static int _slice_count(sg::texture<Traits> const& texture)
    {
        return texture.raw()->array_layers() * (Traits::is_cube ? 6 : 1);
    }

    /// The thread extent of `level` — halved on every axis the shape actually halves.
    ///
    /// Which axis carries the slice is the entry point's business rather than the shape's rank:
    /// `main_1d_array_cs` indexes its slice with `id.y`, every 2D array and cube with `id.z`.
    template <class Traits>
    [[nodiscard]] static extent _extent_of(sg::texture<Traits> const& texture, int level)
    {
        auto e = extent{.x = _mip_extent(texture.width(), level)};
        if constexpr (Traits::dimension == sg::texture_dimension::d1)
        {
            if constexpr (Traits::is_array)
                e.y = _slice_count(texture);
        }
        else if constexpr (Traits::dimension == sg::texture_dimension::d3)
        {
            e.y = _mip_extent(texture.height(), level);
            e.z = _mip_extent(texture.depth(), level); // the one axis that halves with the rest
        }
        else
        {
            e.y = _mip_extent(texture.height(), level);
            if constexpr (Traits::is_array || Traits::is_cube)
                e.z = _slice_count(texture); // a slice is never averaged into another
        }
        return e;
    }

    /// Records one level, reading the program off the routine the caller already acquired.
    static void _dispatch_level(sg::command_list& cmd,
                                box_filter_mipmap_routine const& self,
                                sg::raw_view const& source,
                                sg::raw_view const& target,
                                int x,
                                int y,
                                int z);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    /// The one program this instance is for — its variant is params().
    ///
    /// Only the variants a caller actually asks for are ever instantiated, which is the laziness the old per-variant
    /// cache existed to provide: a caller that only mips 2D textures never reaches the 1D, 3D or array shaders,
    /// because it never acquires those instances.
    std::shared_ptr<mipmap_program const> _program;
};
