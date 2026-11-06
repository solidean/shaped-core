#pragma once

#include <shaped-graphics-language/fwd.hh>

namespace sgl::ir
{
/// Folds what is known and drops the branches that decides, which an unrolled loop is full of.
///
/// `p` must be in SSA form.
/// It keeps every side effect and each operation's float guarantee, and may drop a loop whose back edge is folded away,
/// so a merge block's construct no longer holds: this is for a target with no structured control flow to keep, DXIL.
void simplify(program& p);
} // namespace sgl::ir
