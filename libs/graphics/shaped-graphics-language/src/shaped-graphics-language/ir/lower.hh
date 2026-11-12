#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/emit/impl/plan.hh>
#include <shaped-graphics-language/ir/ir.hh>

namespace sgl::ir
{
/// `plan`'s core tree as a program in `out`, which must be empty.
/// The plan decides every name and every address, as it does for a text target, so what the host binds is one answer.
/// Nothing, or what has no lowering yet, as a phrase for a reader: a construct is never guessed at.
[[nodiscard]] cc::optional<cc::string> lower(emit::impl::plan const& plan, program& out);
} // namespace sgl::ir
