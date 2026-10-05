#include "bindless_tables.hh"

#include <clean-core/common/assert.hh>
#include <sv_shaders.hh> // module `tracer`'s generated groups

using namespace cc::primitive_defines;

namespace
{
/// The suffix of each table's name after the module's `bindless.`, in `bindless_table` order.
constexpr cc::string_view table_names[] = {"textures_1d",   "textures_1d_array",   "textures_2d", "textures_2d_array",
                                           "textures_cube", "textures_cube_array", "textures_3d", "buffers"};

static_assert(sizeof(table_names) / sizeof(table_names[0]) == u32(sv::bindless_table::count_), "one name per table");

/// The module's bindings, checked once against the enum: a table added to one and not the other asserts here.
[[nodiscard]] cc::span<sg::binding const> checked_bindings()
{
    auto const bindings = sv::shaders::tracer::bindless::declared_bindings();
    CC_ASSERT(bindings.size() == isize(sv::bindless_table::count_), "module tracer's `binding bindless` declares one "
                                                                    "binding per sv::bindless_table");
    for (auto i = isize(0); i < bindings.size(); ++i)
    {
        auto const name = cc::string_view(bindings[i].name);
        CC_ASSERT(name.starts_with("bindless.") && name.subview(9) == table_names[i],
                  "module tracer's `binding bindless` declares its tables in sv::bindless_table order");
        CC_ASSERT(bindings[i].count >= 2, "a bindless table needs at least 2 elements — sg reads a count of 1 as a "
                                          "scalar binding");
    }
    return bindings;
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

u32 sv::capacity_of(bindless_table t)
{
    CC_ASSERT(t < bindless_table::count_, "not a bindless table");
    return bindless_bindings()[u32(t)].count;
}
