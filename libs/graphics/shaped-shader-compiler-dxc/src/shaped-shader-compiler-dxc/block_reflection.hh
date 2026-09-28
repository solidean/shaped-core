#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shaped-shader-compiler-dxc/fwd.hh>

/// Where a compiled module puts the values of its blocks, which a caller that wrote the source checks against its own layout.

/// One builtin value of a block: its name, dotted into a nested struct, and its offset from the start.
struct ssc::dxc::reflected_field
{
    cc::string name;
    isize offset = 0;
};

/// A uniform block, a push-constant block, or the element of a storage buffer.
struct ssc::dxc::reflected_block
{
    /// The variable the module reads it through.
    cc::string name;
    /// The bytes one element of a storage buffer takes; 0 for a block.
    isize stride = 0;
    /// Every builtin value, in the module's member order; empty for a storage buffer of a scalar, vector or matrix.
    cc::vector<reflected_field> fields;
};

namespace ssc::dxc
{
/// Every block of a SPIR-V module, read from its Offset and ArrayStride decorations.
/// Needs no DXC, so it works wherever the module came from.
[[nodiscard]] cc::result<cc::vector<reflected_block>> reflect_spirv_blocks(cc::span<byte const> spirv);
} // namespace ssc::dxc
