#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
#include <shaped-graphics-language/ir/ir.hh>

/// One target's whole way from a plan to its binary: lowering, the passes in order, and the writer.
/// Each returns what has no lowering yet as a phrase for a reader, and nothing otherwise.
/// `p` must be empty, and holds what was written, for a reader; `out` is untouched on a gap.
/// Wherever assertions are on, `ir::verify` runs after lowering and after each pass.
namespace sgl::ir
{
/// Keeps its structured control flow to the end, since SPIR-V states it.
[[nodiscard]] cc::optional<cc::string> compile_spirv(emit::impl::plan const& plan, program& p, cc::vector<u32>& out);

/// May fold a construct away, since DXIL states none.
[[nodiscard]] cc::optional<cc::string> compile_dxil(emit::impl::plan const& plan, program& p, cc::vector<byte>& out);
} // namespace sgl::ir
