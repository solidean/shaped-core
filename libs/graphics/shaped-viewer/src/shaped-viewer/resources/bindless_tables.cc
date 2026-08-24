#include "bindless_tables.hh"

#include <clean-core/common/assert.hh>
#include <shaped-graphics/resource/views.hh>

using namespace cc::primitive_defines;

namespace
{
/// Everything that differs per table, in one place, so adding a table is one row rather than four switches.
struct table_traits
{
    cc::string_view name;
    sg::binding_type type = sg::binding_type::readonly_texture;
    sg::texture_view_dimension dimension = sg::texture_view_dimension::tex_2d; ///< ignored for `buffers`
    u32 default_count = 0;
};

// Indexed by bindless_table, so the order must match the enum's.
constexpr table_traits traits[] = {
    {.name = "gBindlessTextures1D", .dimension = sg::texture_view_dimension::tex_1d, .default_count = 64},
    {.name = "gBindlessTextures1DArray", .dimension = sg::texture_view_dimension::tex_1d_array, .default_count = 32},
    {.name = "gBindlessTextures2D", .dimension = sg::texture_view_dimension::tex_2d, .default_count = 4096},
    {.name = "gBindlessTextures2DArray", .dimension = sg::texture_view_dimension::tex_2d_array, .default_count = 256},
    {.name = "gBindlessTexturesCube", .dimension = sg::texture_view_dimension::cube, .default_count = 64},
    {.name = "gBindlessTexturesCubeArray", .dimension = sg::texture_view_dimension::cube_array, .default_count = 16},
    {.name = "gBindlessTextures3D", .dimension = sg::texture_view_dimension::tex_3d, .default_count = 128},
    {.name = "gBindlessBuffers", .type = sg::binding_type::readonly_raw_buffer, .default_count = 4096},
};

static_assert(sizeof(traits) / sizeof(traits[0]) == u32(sv::bindless_table::count_), "one row per table");

[[nodiscard]] table_traits const& traits_of(sv::bindless_table t)
{
    CC_ASSERT(t < sv::bindless_table::count_, "not a bindless table");
    return traits[u32(t)];
}
} // namespace

cc::string_view sv::name_of(bindless_table t)
{
    return traits_of(t).name;
}

u32 sv::space_of(bindless_table t)
{
    CC_ASSERT(t < bindless_table::count_, "not a bindless table");
    // One space per table, numbered from 1 — space 0 is left to the ordinary per-pass bindings.
    return u32(t) + 1;
}

cc::vector<sv::bindless_table_budget> sv::default_bindless_tables()
{
    auto r = cc::vector<bindless_table_budget>();
    r.reserve(isize(bindless_table::count_));
    for (auto i = u32(0); i < u32(bindless_table::count_); ++i)
        r.push_back({.table = bindless_table(i), .count = traits[i].default_count});
    return r;
}

cc::vector<sg::binding> sv::make_bindless_bindings(bindless_config const& cfg)
{
    auto r = cc::vector<sg::binding>();
    r.reserve(cfg.tables.size());
    for (auto const& b : cfg.tables)
    {
        if (b.count == 0)
            continue; // the table is not declared at all, and a shader naming it fails group creation

        CC_ASSERT(b.count >= 2, "a bindless table needs at least 2 elements — sg reads a count of 1 as a scalar "
                                "binding");
        auto const& t = traits_of(b.table);

        // Every table sits at index 0 of its own space, so a category is addressed with no register-offset math.
        auto binding = sg::binding{.name = cc::string(t.name),
                                   .space = space_of(b.table),
                                   .index = 0,
                                   .count = b.count,
                                   .type = t.type};
        if (t.type == sg::binding_type::readonly_texture)
            binding.texture_dimension = t.dimension;
        r.push_back(cc::move(binding));
    }
    return r;
}
