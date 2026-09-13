#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-viewer/fwd.hh>

/// The bindless tables sv declares, and the binding-group layout they turn into.
///
/// The layout is hand-written rather than reflected out of a shader: sv owns the contract, so the manager is
/// constructible before any shader has compiled, and a routine's init order cannot decide whether a resource
/// can be acquired.
/// What a shader must do in exchange is declare the names below, in the space this header names.

/// One bindless table — one array binding, one shader-visible dimension.
///
/// The split is by *view dimension* rather than by meaning: a table's elements all have to satisfy one binding,
/// and a shader indexes `Texture2D` and `TextureCube` arrays separately whatever the textures are used for.
/// So "albedo" and "normal" share `textures_2d`, and nothing here knows either name.
enum class sv::bindless_table : sv::u8
{
    textures_1d,
    textures_1d_array,
    textures_2d,
    textures_2d_array,
    textures_cube,
    textures_cube_array,
    textures_3d,
    buffers, ///< byte-address (`ByteAddressBuffer`), the one buffer shape sv binds bindlessly

    count_ ///< not a table: the number of them, for arrays indexed by table
};

/// How many elements one table holds.
///
/// A count of 0 omits the table from the layout entirely — a shader still declaring it then fails group
/// creation, which is the failure that names the problem.
/// A non-zero count below 2 asserts: sg reads a count of 1 as a *scalar* binding, which has no vacant elements
/// and so cannot back a table at all.
struct sv::bindless_table_budget
{
    bindless_table table = bindless_table::textures_2d;
    u32 count = 0;
};

namespace sv
{
/// The shader-visible binding name of `t` — `gBindlessTextures2D` and friends.
///
/// The `gBindless` prefix is deliberate: sv's other shader globals are unprefixed (`Vertices`, `Materials`,
/// `frame`), so the prefix is what marks a name as a manager-owned table rather than an ordinary binding.
[[nodiscard]] cc::string_view name_of(bindless_table t);

/// The tables as HLSL, as one annotated namespace slib's binding pass assigns the addresses of.
///
/// This is the single text both halves read: `make_bindless_bindings` parses it for the C++ layout, and
/// `generate_material_shader` embeds it in every permutation.
/// So the addresses the shader uses and the addresses the root signature declares are the same parse rather than
/// two readings of one convention -- which is what they were when both computed `index = 0, space = table + 1`
/// by hand.
///
/// EVERY budgeted table is declared, whether or not the material touches it.
/// The pass numbers a group by declaration order and an array consumes one index per element, so a permutation
/// declaring a subset would land on different registers than the layout built from the whole set -- the shader
/// reading `t0` where the root signature put that table at `t96`, with nothing to report it.
[[nodiscard]] cc::string bindless_declarations(bindless_config const& cfg);

/// The group the manager's tables are declared in, and the slot `pipeline_layout_description::groups` binds
/// them at.
///
/// 1, because the trace's own bindings are group 0.
inline constexpr int bindless_group = 1;
inline constexpr cc::string_view bindless_namespace = "sv_bindless";

/// The group a material permutation declares its samplers in, and the namespace it declares them under.
///
/// A permutation is generated and compiled at runtime, so its samplers cannot join a group any package declares
/// — but they do not have to be hand-numbered either.
/// They are a group of their own, slib's binding pass writes their addresses, and this is the number a
/// permutation's `#pragma sc group` carries and the slot `pipeline_layout_description::groups` binds it at.
///
/// 2, because the trace's own bindings are group 0 and the manager's tables are the group bound after them.
inline constexpr int material_sampler_group = 2;
inline constexpr cc::string_view material_sampler_namespace = "sv_material_samplers";

/// The default table set: every table, with the budgets documented on `bindless_config`.
[[nodiscard]] cc::vector<bindless_table_budget> default_bindless_tables();
} // namespace sv

/// Which tables a `gpu_resource_manager` declares, and how large each one is.
///
/// The defaults are starting points chosen to be generous for a viewer-sized working set, not measurements —
/// tune them against real content, per use case, through `gpu_resource_manager_config`.
struct sv::bindless_config
{
    cc::vector<bindless_table_budget> tables = default_bindless_tables();
};

namespace sv
{
/// The binding-group layout `cfg` describes, in table order.
///
/// Tables budgeted at 0 are absent from the result.
/// Every entry carries its name, space, `count` and — for the texture tables — the `texture_dimension` a
/// backend needs to synthesize a dimension-correct null descriptor for a vacant element.
[[nodiscard]] cc::vector<sg::binding> make_bindless_bindings(bindless_config const& cfg);
} // namespace sv
