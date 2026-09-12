#include "bindless_tables.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/resource/views.hh>
#include <shaped-shader-library/binding/binding_groups.hh>

using namespace cc::primitive_defines;

namespace
{
/// Everything that differs per table, in one place, so adding a table is one row rather than four switches.
struct table_traits
{
    cc::string_view name;
    cc::string_view hlsl_type; ///< how the table is spelled in the declaration the pass reads
    sg::binding_type type = sg::binding_type::readonly_texture;
    sg::texture_view_dimension dimension = sg::texture_view_dimension::tex_2d; ///< ignored for `buffers`
    u32 default_count = 0;
};

// Indexed by bindless_table, so the order must match the enum's.
constexpr table_traits traits[] = {
    {.name = "gBindlessTextures1D",
     .hlsl_type = "Texture1D",
     .dimension = sg::texture_view_dimension::tex_1d,
     .default_count = 64},
    {.name = "gBindlessTextures1DArray",
     .hlsl_type = "Texture1DArray",
     .dimension = sg::texture_view_dimension::tex_1d_array,
     .default_count = 32},
    {.name = "gBindlessTextures2D",
     .hlsl_type = "Texture2D",
     .dimension = sg::texture_view_dimension::tex_2d,
     .default_count = 4096},
    {.name = "gBindlessTextures2DArray",
     .hlsl_type = "Texture2DArray",
     .dimension = sg::texture_view_dimension::tex_2d_array,
     .default_count = 256},
    {.name = "gBindlessTexturesCube",
     .hlsl_type = "TextureCube",
     .dimension = sg::texture_view_dimension::cube,
     .default_count = 64},
    {.name = "gBindlessTexturesCubeArray",
     .hlsl_type = "TextureCubeArray",
     .dimension = sg::texture_view_dimension::cube_array,
     .default_count = 16},
    {.name = "gBindlessTextures3D",
     .hlsl_type = "Texture3D",
     .dimension = sg::texture_view_dimension::tex_3d,
     .default_count = 128},
    {.name = "gBindlessBuffers",
     .hlsl_type = "ByteAddressBuffer",
     .type = sg::binding_type::readonly_raw_buffer,
     .default_count = 4096},
};

static_assert(sizeof(traits) / sizeof(traits[0]) == u32(sv::bindless_table::count_), "one row per table");

[[nodiscard]] table_traits const& traits_of(sv::bindless_table t)
{
    CC_ASSERT(t < sv::bindless_table::count_, "not a bindless table");
    return traits[u32(t)];
}

/// The table a declared binding name belongs to — the reverse of `name_of`.
[[nodiscard]] sv::bindless_table table_named(cc::string_view name)
{
    for (auto i = u32(0); i < u32(sv::bindless_table::count_); ++i)
        if (traits[i].name == name)
            return sv::bindless_table(i);

    CC_ASSERT(false, "a binding parsed out of sv's own declarations names no table");
    return sv::bindless_table::textures_2d;
}
} // namespace

cc::string_view sv::name_of(bindless_table t)
{
    return traits_of(t).name;
}

cc::vector<sv::bindless_table_budget> sv::default_bindless_tables()
{
    auto r = cc::vector<bindless_table_budget>();
    r.reserve(isize(bindless_table::count_));
    for (auto i = u32(0); i < u32(bindless_table::count_); ++i)
        r.push_back({.table = bindless_table(i), .count = traits[i].default_count});
    return r;
}

cc::string sv::bindless_declarations(bindless_config const& cfg)
{
    auto src = cc::string();
    cc::format_append(src, "#pragma sc group {}\nnamespace {}\n{{\n", bindless_group, bindless_namespace);
    for (auto const& b : cfg.tables)
    {
        if (b.count == 0)
            continue; // the table is not declared at all, and a shader naming it fails to compile

        CC_ASSERT(b.count >= 2, "a bindless table needs at least 2 elements — sg reads a count of 1 as a scalar "
                                "binding");
        auto const& t = traits_of(b.table);
        cc::format_append(src, "    {} {}[{}];\n", t.hlsl_type, t.name, b.count);
    }

    // Unqualified below, as the names were when each table was a global with a hand-written register.
    cc::format_append(src, "}}\nusing namespace {};\n", bindless_namespace);
    return src;
}

cc::vector<sg::binding> sv::make_bindless_bindings(bindless_config const& cfg)
{
    // Parsed rather than constructed, so the layout's addresses ARE the shader's: the same text goes into every
    // permutation, and the pass that numbers it there is the one that numbered it here.
    // Building these by hand is what made `index = 0, space = table + 1` a rule two files had to keep.
    auto parsed = slib::parse_binding_groups(bindless_declarations(cfg));
    CC_ASSERT(parsed.has_value(), "sv's own bindless declarations must parse");

    auto& groups = parsed.value().groups;
    if (groups.empty())
        return {}; // every table budgeted at zero, so the namespace is empty and the pass reports no group

    auto r = cc::move(groups[0].bindings);
    for (auto& binding : r)
    {
        auto const& t = traits_of(table_named(binding.name));

        // The dimension is sg's rather than HLSL's: a backend needs it to synthesize a dimension-correct null
        // descriptor for a vacant element, and the pass reads it off the declared type for a scalar binding only.
        if (t.type == sg::binding_type::readonly_texture)
            binding.texture_dimension = t.dimension;
    }
    return r;
}
