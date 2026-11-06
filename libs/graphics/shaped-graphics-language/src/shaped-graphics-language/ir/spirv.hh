#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/ir/ir.hh>

namespace sgl::ir
{
/// `p` as a SPIR-V 1.6 module for Vulkan 1.3, one 32-bit word after another.
/// Every capability and extension it declares is one `p` uses, so what a device needs follows from the program.
/// Deterministic: equal programs give equal words.
[[nodiscard]] cc::vector<u32> write_spirv(program const& p);
} // namespace sgl::ir
