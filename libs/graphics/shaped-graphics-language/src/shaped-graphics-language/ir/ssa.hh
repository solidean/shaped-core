#pragma once

#include <shaped-graphics-language/fwd.hh>

namespace sgl::ir
{
/// Rewrites `p` so a local only ever reached whole or at constant indices is no variable any more.
///
/// Each scalar of such a local becomes its own value, with a phi wherever two definitions meet.
/// A local an index reaches at run time stays a variable, as does anything an operation other than a load or a store
/// touches through it: this form says which, and a writer gives what is left the memory it needs.
void construct_ssa(program& p);
} // namespace sgl::ir
