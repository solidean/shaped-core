#include "bindless_tables.hh"

#include <clean-core/common/assert.hh>
#include <sv_shaders.hh> // module `tracer`'s generated groups

using namespace cc::primitive_defines;

namespace
{
using bindless_arrays = sv::shaders::tracer::bindless::arrays;

/// The texture tables' generated handles, in `bindless_table` order; `buffers` follows them.
constexpr sg::texture_array_binding texture_arrays[] = {
    bindless_arrays::textures_1d,       bindless_arrays::textures_1d_array, bindless_arrays::textures_2d,
    bindless_arrays::textures_2d_array, bindless_arrays::textures_cube,     bindless_arrays::textures_cube_array,
    bindless_arrays::textures_3d,
};

static_assert(sizeof(texture_arrays) / sizeof(texture_arrays[0]) == u32(sv::bindless_table::buffers),
              "one handle per texture table, and the buffer table last");

/// The module's tables, checked once against the enum: a table added to one and not the other asserts here.
/// The material sampler palette follows them in the binding, and is all that may.
[[nodiscard]] cc::span<sg::binding const> checked_bindings()
{
    auto const bindings = sv::shaders::tracer::bindless::declared_bindings();
    auto const tables = isize(sv::bindless_table::count_);
    CC_ASSERT(bindings.size() >= tables, "`binding bindless` declares a binding per sv::bindless_table");
    for (auto i = isize(0); i < bindings.size(); ++i)
    {
        auto const name = cc::string_view(bindings[i].name);
        if (i >= tables)
        {
            auto const is_sampler = bindings[i].type == sg::binding_type::sampler;
            CC_ASSERT(is_sampler, "`binding bindless` declares samplers alone after its tables");
            continue;
        }
        auto const expected
            = i < isize(sv::bindless_table::buffers) ? texture_arrays[i].name : bindless_arrays::buffers.name;
        CC_ASSERT(name == expected, "module tracer's `binding bindless` declares its tables in sv::bindless_table "
                                    "order");
        CC_ASSERT(bindings[i].count >= 2, "a bindless table needs at least 2 elements — sg reads a count of 1 as a "
                                          "scalar binding");
    }
    return bindings.subspan(cc::offset_size{.offset = 0, .size = tables});
}
} // namespace

cc::span<sg::binding const> sv::bindless_bindings()
{
    static auto const bindings = checked_bindings();
    return bindings;
}

cc::string_view sv::name_of(bindless_table t)
{
    CC_ASSERT(t < bindless_table::count_, "not a bindless table");
    return bindless_bindings()[u32(t)].name;
}

sg::texture_array_binding sv::texture_array_of(bindless_table t)
{
    CC_ASSERT(t < bindless_table::buffers, "not a texture table");
    return texture_arrays[u32(t)];
}

sg::buffer_array_binding sv::buffer_array()
{
    return bindless_arrays::buffers;
}

u32 sv::capacity_of(bindless_table t)
{
    CC_ASSERT(t < bindless_table::count_, "not a bindless table");
    return bindless_bindings()[u32(t)].count;
}
