#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/fwd.hh>

/// Where a stage's elements stand in DXIL: the one place that knows a row.
///
/// D3D12 links two stages by name, then register, then mask, so a stage of ours links against a stage DXC compiled only
/// if this places every element as DXC's default `-pack-prefix-stable` does (EMIT-235).

/// Which table of a container an element stands in.
enum class sgl::ir::dxil::signature_table : sgl::u8
{
    /// `ISG1`.
    input,
    /// `OSG1`.
    output,
    /// `PSG1`: a tessellation stage's patch constants, or a mesh stage's per-primitive values.
    patch,
};

/// One element as a container states it.
struct sgl::ir::dxil::signature_element
{
    signature_table table = signature_table::input;
    /// Its position in `program::stage_elements`.
    u32 element = 0;
    cc::string name;
    u32 semantic_index = 0;
    /// `D3D_NAME`, 0 for an element that is no system value.
    u32 system_value = 0;
    /// `DxilProgramSigCompType`.
    u32 component_type = 0;
    /// -1 for a system element with no register, which DXIL states with no row: `SV_Depth`, `SV_SampleIndex`.
    i32 row = -1;
    u32 first_column = 0;
    u32 lanes = 0;
    interpolation mode = {};
    /// A later factor of one tessellation factor array, which `ISG1` and its like state row by row and `dx.sig` and
    /// `PSV0` state as one element of as many rows.
    bool continues = false;

    [[nodiscard]] u32 mask() const { return ((1u << lanes) - 1u) << first_column; }
    [[nodiscard]] bool operator==(signature_element const& rhs) const = default;
};

namespace sgl::ir::dxil
{
/// Every element of `p` that stands in a signature, by table and then in declared order.
///
/// An element DXIL reads through an operation of its own stands in none: `SV_ViewID`, a compute stage's.
/// `p.stage_elements` must stand in declared order, a part's members then its stage inputs, since that order is the rule.
[[nodiscard]] cc::vector<signature_element> pack_signature(program const& p);
} // namespace sgl::ir::dxil
