#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/fwd.hh>

/// What every `ir::program` must hold, whichever pass last touched it.
///
/// A writer takes a program on trust and nothing validates a shipped compile, so this is the one check that stands
/// between a pass's mistake and a driver.
/// It trusts nothing it is checking: reachability and dominance are worked out from the terminators.
enum class sgl::ir::control_flow : sgl::u8
{
    /// Every header's merge and continue targets still hold, so a SPIR-V writer can state them.
    structured,
    /// A pass may have folded a construct away, so only reducibility remains, which is all DXIL asks.
    unstructured,
};

namespace sgl::ir
{
/// The first thing wrong with `p`, naming its block and operation, or nothing.
///
/// Covers terminators, ids, operand roles, the types each family of operation takes, phis against their block's
/// predecessors, and that a definition dominates its uses.
/// A block no walk reaches is checked as a table but not for dominance, since nothing runs it.
/// Linear in the program's size but allocating, so a compile runs it only where assertions are on.
[[nodiscard]] cc::optional<cc::string> verify(program const& p, control_flow flow);
} // namespace sgl::ir
