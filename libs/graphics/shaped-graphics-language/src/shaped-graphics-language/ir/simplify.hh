#pragma once

#include <shaped-graphics-language/fwd.hh>

namespace sgl::ir
{
/// Folds what is known and drops the branches that decides, which an unrolled loop is full of.
///
/// `p` must be in SSA form.
/// It keeps every side effect and each operation's float guarantee.
/// `unstructured` also folds a known branch and drops what no walk reaches, which may drop a loop whose back edge is
/// folded away, so a merge block's construct no longer holds: that is for DXIL.
/// `structured` folds a selection's known branch alone, so SPIR-V's merge and continue targets still hold.
/// It drops what no walk reaches but a block a header that stays names, which is left a label and `unreachable`.
void simplify(program& p, control_flow flow);

/// `simplify` for DXIL, `unstructured`.
void simplify(program& p);
} // namespace sgl::ir
