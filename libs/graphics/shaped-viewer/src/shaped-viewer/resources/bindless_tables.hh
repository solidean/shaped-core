#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
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

/// The register space every bindless table lives in, one per table, so a category is addressed with no
/// register-offset math and adding a table never renumbers another.
[[nodiscard]] u32 space_of(bindless_table t);

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
