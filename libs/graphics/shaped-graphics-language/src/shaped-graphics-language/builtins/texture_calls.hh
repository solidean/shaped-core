#pragma once

#include <shaped-graphics-language/check/resources.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/interpret/scalar.hh>

/// What a texture or image builtin is, as its record's `spelling::data` packs it, for the writers and the interpreter.

enum class sgl::builtins::texture::op : sgl::u8
{
    sample,
    sample_level,
    sample_bias,
    sample_grad,
    gather,
    sample_compare,
    sample_compare_level,
    gather_compare,
    load,
    size,
    layer_count,
    level_count,
    sample_count,
    image_load,
    image_store,
    image_size,
    image_layer_count,
    /// A test's statement of one texel of what its run binds (EVAL-127), which no target writes.
    test_texel,
};

/// One texture call, packed into a record's `spelling::data`.
struct sgl::builtins::texture::call
{
    op what = op::sample;
    check::texture_shape where = check::texture_shape::d2;
    /// The texel's width, 1 to 4; what a sample of a depth texture gives is 1.
    int width = 4;
    check::value_kind kind = check::value_kind::scalar_float;
    bool is_depth = false;
    bool has_offset = false;
    /// A `test_texel` of an image, which has no level.
    bool is_image = false;

    [[nodiscard]] u32 pack() const
    {
        return u32(what) | u32(where) << 5 | u32(width) << 9 | u32(kind) << 12 | u32(is_depth) << 15
             | u32(has_offset) << 16 | u32(is_image) << 17;
    }
    [[nodiscard]] static call unpack(u32 d)
    {
        return {.what = op(d & 31u),
                .where = check::texture_shape((d >> 5) & 15u),
                .width = int((d >> 9) & 7u),
                .kind = check::value_kind((d >> 12) & 7u),
                .is_depth = ((d >> 15) & 1u) != 0,
                .has_offset = ((d >> 16) & 1u) != 0,
                .is_image = ((d >> 17) & 1u) != 0};
    }
};

/// Positions in a call's arguments; -1 for one the call does not take.
struct sgl::builtins::texture::argument_layout
{
    int sampler = -1;
    int layer = -1;
    /// The level, the bias, the component, the reference, the sample or the value, by what the call is.
    int first = -1;
    /// The second gradient, or a compare's level.
    int second = -1;
    int offset = -1;
    /// A `test_texel`'s level, its multisampled texture's sample, and its cube's face.
    int level = -1;
    int sample = -1;
    int face = -1;
};

/// The shape's coordinates: 1, 2 or 3, a cube's direction 3; and whether it has layers, faces or samples.
struct sgl::builtins::texture::shape_axes
{
    int dim = 2;
    bool is_array = false;
    bool is_cube = false;
    bool is_ms = false;
};

namespace sgl::builtins::texture
{

[[nodiscard]] shape_axes axes_of(check::texture_shape s);
[[nodiscard]] bool takes_sampler(op o);
[[nodiscard]] argument_layout layout_of(call const& c);
} // namespace sgl::builtins::texture
