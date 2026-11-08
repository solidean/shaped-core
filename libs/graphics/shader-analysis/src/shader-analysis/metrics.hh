#pragma once

#include <babel-data/fwd.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <shader-analysis/fwd.hh>
#include <shader-analysis/op_kind.hh>

// What a module costs, function by function, read off its instructions alone: SPIR-V, or DXIL's LLVM 3.7.
//
// THESE ARE ESTIMATES FOR COMPARING TWO MODULES OF ONE ENTRY POINT, not a model of any GPU.
// Register pressure is the most 32-bit words of SSA values live at once, a bool taking one and a pointer none.
// A driver's allocator does better or worse than that, but two modules of one entry point it treats alike.
// It splits by uniformity: a value is varying, a vector word, where it derives from a thread's own input, a load from
// a function's variable, an atomic, a lane operation, or a phi that a branch on a varying value parted the lanes for.
// Everything else, a wave's own answer included, is a scalar word.
// The critical path is the longest chain of dependent operations within one block, weighted by `latency_of`.
//
// A SPIR-V operand is an id or a literal, which only the full grammar tells apart; shan knows the literal positions of
// the opcodes a shader's body is made of, and counts any other operand a use only where it names a value of the function.
//
//   auto const m = shan::analyze(babel::spirv::read(words).value()).value();
//   m.total.max_live_words;                          // the register pressure of the worst function
//   m.total.kinds[isize(shan::op_kind::load)];       // every load of the module

/// One function's metrics, or a module's totals: counts summed, `max_live_words` and `critical_path` the worst.
struct shan::function_metrics
{
    /// `OpName`'s, or `%` and the id where the module names it nothing.
    cc::string name;
    u32 id = 0;
    /// Operations that run: every instruction of the body but labels, merges and declarations.
    i32 instructions = 0;
    i32 kinds[k_op_kind_count] = {};
    i32 blocks = 0;
    i32 loops = 0;
    i32 selections = 0;
    i32 calls = 0;
    /// The words of the function's own variables, which a driver keeps in registers or in memory.
    i32 variable_words = 0;
    /// The most 32-bit words of SSA values live at one point.
    i32 max_live_words = 0;
    /// The same, of the values that may differ between a wave's threads, which a GPU keeps in vector registers.
    i32 max_live_vector_words = 0;
    /// The same, of the values the whole wave shares, which a GPU may keep in scalar registers.
    i32 max_live_scalar_words = 0;
    /// The longest chain of dependent operations in one block, in `latency_of` units.
    i32 critical_path = 0;

    [[nodiscard]] i32 count_of(op_kind kind) const { return kinds[isize(kind)]; }
};

struct shan::module_metrics
{
    cc::vector<function_metrics> functions;
    function_metrics total;
    /// The entry points' names, as `OpEntryPoint` states them.
    cc::vector<cc::string> entry_points;
};

namespace shan
{
/// The metrics of every function of `m`; an error where a function's body is malformed, a block with no terminator.
[[nodiscard]] cc::result<module_metrics> analyze(babel::spirv::module const& m);

/// The metrics of every function body of a DXIL module.
/// A `dx.op` call is the kind its operation class is: `rawBufferLoad` a load, `unary` and `dot3` extended,
/// `createHandle` and `annotateHandle` an address; an `llvm.*` intrinsic is a declaration.
/// A selection is a conditional branch or a switch, and a loop an edge back to a block dominating its source.
[[nodiscard]] cc::result<module_metrics> analyze(babel::llvm37::module const& m);

/// A SPIR-V module or a DXIL container, told apart by its first bytes, read and analyzed.
[[nodiscard]] cc::result<module_metrics> analyze_binary(cc::span<byte const> bytes);

/// The metrics as text, one line a metric, the totals first and each function after.
[[nodiscard]] cc::string to_text(module_metrics const& m);
} // namespace shan
