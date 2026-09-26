#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/check/ids.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>

/// A `let` that writes no type, and the type the check gave it: what an editor shows as `let x : int`.
struct sgl::unannotated_binding
{
    /// The bound name; an annotation is written right after it.
    source_span name;
    check::type_id type = check::type_id::none;

    constexpr bool operator==(unannotated_binding const&) const = default;
};

namespace sgl
{
/// Every `let` of `file` that binds one name and writes no type, in source order.
/// A binding the check did not reach, or gave the error type, is left out, since there is no type worth showing.
[[nodiscard]] cc::vector<unannotated_binding> unannotated_bindings(ast::file_ast const& ast,
                                                                   check::checked_module const& m,
                                                                   i32 file);
} // namespace sgl
