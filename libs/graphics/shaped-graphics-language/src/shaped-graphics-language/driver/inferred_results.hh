#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/check/ids.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>

/// A function or property that writes no return type, and the one the check inferred from its body: what an editor
/// shows as `fun half(x: float) -> float => x * 0.5`.
struct sgl::inferred_result
{
    /// The `=>` of the body; `-> type` is written right in front of it.
    source_span arrow;
    check::type_id type = check::type_id::none;
    /// A property of a type body takes no `-> type` (AST-81), so there the type can be shown and not written.
    bool is_writable = true;

    constexpr bool operator==(inferred_result const&) const = default;
};

namespace sgl
{
/// Every function and property of `file` whose return type is inferred, which is one with an arrow body and no `->`,
/// in source order.
/// A block body without `->` returns `void` rather than an inferred type (CHK-121), so it is not among them.
/// One that failed, or whose type is the error type, is left out, since there is no type worth showing.
[[nodiscard]] cc::vector<inferred_result> inferred_results(parsed_file const& file,
                                                           ast::file_ast const& ast,
                                                           check::checked_module const& m,
                                                           i32 file_index);
} // namespace sgl
