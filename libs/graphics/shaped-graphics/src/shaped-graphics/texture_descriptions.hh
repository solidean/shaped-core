#pragma once

#include <shaped-graphics/texture.hh> // texture<Traits> + the shape typedefs

/// Shape-specific texture descriptions — the input to the typed `create_texture_*` factories on the
/// context scopes (context.persistent.hh / context.transient.hh). Each exposes only the parameters that
/// are *free* for its shape (a plain 2D texture has no depth / layers / samples), so a nonsensical field
/// is a compile error rather than an ignored value. `to_texture_description()` expands one into the full
/// `texture_description` by filling in the shape-fixed fields; the factory then creates the raw_texture
/// and wraps it in the matching `texture<Traits>` (named by the `texture_type` alias).

namespace sg
{
struct texture_1d_description
{
    pixel_format format = pixel_format::undefined;
    int width = 1;
    int mip_levels = 1;
    texture_usage usage = texture_usage::none;

    using texture_type = texture_1d;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d1,
                .width = width,
                .mip_levels = mip_levels,
                .usage = usage};
    }
};

struct texture_2d_description
{
    pixel_format format = pixel_format::undefined;
    int width = 1;
    int height = 1;
    int mip_levels = 1;
    texture_usage usage = texture_usage::none;

    using texture_type = texture_2d;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d2,
                .width = width,
                .height = height,
                .mip_levels = mip_levels,
                .usage = usage};
    }
};

struct texture_3d_description
{
    pixel_format format = pixel_format::undefined;
    int width = 1;
    int height = 1;
    int depth = 1;
    int mip_levels = 1;
    texture_usage usage = texture_usage::none;

    using texture_type = texture_3d;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d3,
                .width = width,
                .height = height,
                .depth = depth,
                .mip_levels = mip_levels,
                .usage = usage};
    }
};

struct texture_cube_description
{
    pixel_format format = pixel_format::undefined;
    int size = 1; ///< edge length; cube faces are square (width == height == size)
    int mip_levels = 1;
    texture_usage usage = texture_usage::none;

    using texture_type = texture_cube;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d2,
                .width = size,
                .height = size,
                .mip_levels = mip_levels,
                .is_cube = true,
                .usage = usage};
    }
};

struct texture_1d_array_description
{
    pixel_format format = pixel_format::undefined;
    int width = 1;
    int mip_levels = 1;
    int array_layers = 1;
    texture_usage usage = texture_usage::none;

    using texture_type = texture_1d_array;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d1,
                .width = width,
                .mip_levels = mip_levels,
                .array_layers = array_layers,
                .usage = usage};
    }
};

struct texture_2d_array_description
{
    pixel_format format = pixel_format::undefined;
    int width = 1;
    int height = 1;
    int mip_levels = 1;
    int array_layers = 1;
    texture_usage usage = texture_usage::none;

    using texture_type = texture_2d_array;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d2,
                .width = width,
                .height = height,
                .mip_levels = mip_levels,
                .array_layers = array_layers,
                .usage = usage};
    }
};

struct texture_cube_array_description
{
    pixel_format format = pixel_format::undefined;
    int size = 1; ///< edge length; cube faces are square (width == height == size)
    int mip_levels = 1;
    int cube_count = 1; ///< number of cubes (the backend expands to 6 * cube_count faces)
    texture_usage usage = texture_usage::none;

    using texture_type = texture_cube_array;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d2,
                .width = size,
                .height = size,
                .mip_levels = mip_levels,
                .array_layers = cube_count,
                .is_cube = true,
                .usage = usage};
    }
};

struct texture_2d_ms_description
{
    pixel_format format = pixel_format::undefined;
    int width = 1;
    int height = 1;
    int sample_count = 1; ///< must be > 1 (multisampled); a single mip level
    texture_usage usage = texture_usage::none;

    using texture_type = texture_2d_ms;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d2,
                .width = width,
                .height = height,
                .sample_count = sample_count,
                .usage = usage};
    }
};

struct texture_2d_array_ms_description
{
    pixel_format format = pixel_format::undefined;
    int width = 1;
    int height = 1;
    int array_layers = 1;
    int sample_count = 1; ///< must be > 1 (multisampled); a single mip level
    texture_usage usage = texture_usage::none;

    using texture_type = texture_2d_array_ms;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d2,
                .width = width,
                .height = height,
                .array_layers = array_layers,
                .sample_count = sample_count,
                .usage = usage};
    }
};

struct texture_cube_ms_description
{
    pixel_format format = pixel_format::undefined;
    int size = 1;         ///< edge length; cube faces are square (width == height == size)
    int sample_count = 1; ///< must be > 1 (multisampled); a single mip level
    texture_usage usage = texture_usage::none;

    using texture_type = texture_cube_ms;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d2,
                .width = size,
                .height = size,
                .sample_count = sample_count,
                .is_cube = true,
                .usage = usage};
    }
};

struct texture_cube_array_ms_description
{
    pixel_format format = pixel_format::undefined;
    int size = 1;         ///< edge length; cube faces are square (width == height == size)
    int cube_count = 1;   ///< number of cubes (the backend expands to 6 * cube_count faces)
    int sample_count = 1; ///< must be > 1 (multisampled); a single mip level
    texture_usage usage = texture_usage::none;

    using texture_type = texture_cube_array_ms;

    [[nodiscard]] texture_description to_texture_description() const
    {
        return {.format = format,
                .dimension = texture_dimension::d2,
                .width = size,
                .height = size,
                .array_layers = cube_count,
                .sample_count = sample_count,
                .is_cube = true,
                .usage = usage};
    }
};
} // namespace sg
