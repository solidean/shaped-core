#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-viewer/fwd.hh>

/// The bindless tables sv declares, and the binding-group layout they turn into.
///
/// **Module `tracer`'s `binding bindless` (shaders/sgl/tracer_bindings.sgl) is the one declaration.**
/// The manager builds its staging group over that binding's layout, so the group a trace binds is the very layout the
/// tracer's pipeline was compiled against, names and counts included.
/// So a table's size is the module's to change, and changing it is an edit to that file rather than a config.

/// One bindless table — one array binding, one shader-visible dimension.
///
/// The split is by *view dimension* rather than by meaning: a table's elements all have to satisfy one binding,
/// and a shader indexes 2D and cube arrays separately whatever the textures are used for.
/// So "albedo" and "normal" share `textures_2d`, and nothing here knows either name.
/// The order is the module's declaration order, which `bindless_bindings` asserts.
enum class sv::bindless_table : sv::u8
{
    textures_1d,
    textures_1d_array,
    textures_2d,
    textures_2d_array,
    textures_cube,
    textures_cube_array,
    textures_3d,
    buffers, ///< raw bytes, the one buffer shape sv binds bindlessly

    count_ ///< not a table: the number of them, for arrays indexed by table
};

namespace sv
{
/// The bindings of module `tracer`'s `binding bindless`, in `bindless_table` order — what the manager's group is laid out as.
[[nodiscard]] cc::span<sg::binding const> bindless_bindings();

/// The shader-visible binding name of `t` — `bindless.textures_2d` and friends, as the module's reflection names it.
/// That name is what sg resolves an access declaration's footprint by.
[[nodiscard]] cc::string_view name_of(bindless_table t);

/// How many elements table `t` holds, as the module declares it.
[[nodiscard]] u32 capacity_of(bindless_table t);

/// The group the manager's tables are bound at in the tracer's pipeline layout: 1, after the trace's own group 0.
inline constexpr int bindless_group = 1;
} // namespace sv
