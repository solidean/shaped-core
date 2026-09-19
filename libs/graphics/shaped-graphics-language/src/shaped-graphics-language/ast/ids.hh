#pragma once

#include <shaped-graphics-language/fwd.hh>

/// An AST node is named by its position in the `file_ast` array that holds it, one id type per array.
/// `none` is the absent link, and `file_ast::at` is how an id becomes a node.

/// A position in `file_ast::exprs`.
enum class sgl::ast::expr_id : sgl::i32
{
    none = -1
};

/// A position in `file_ast::stmts`.
enum class sgl::ast::stmt_id : sgl::i32
{
    none = -1
};

/// A position in `file_ast::decls`.
enum class sgl::ast::decl_id : sgl::i32
{
    none = -1
};

/// A position in `file_ast::fields`.
enum class sgl::ast::field_id : sgl::i32
{
    none = -1
};

/// A run of consecutive entries of the one `file_ast` side array that holds `T`s.
/// `first` is a position and names nothing when `count` is zero.
template <class T>
struct sgl::ast::range_of
{
    u32 first = 0;
    u32 count = 0;

    [[nodiscard]] constexpr bool empty() const { return count == 0; }

    constexpr bool operator==(range_of const&) const = default;
};

namespace sgl::ast
{

[[nodiscard]] constexpr bool is_valid(expr_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(stmt_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(decl_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(field_id id)
{
    return i32(id) >= 0;
}

/// The position an id names.
/// `id` must be valid.
[[nodiscard]] constexpr isize index_of(expr_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(stmt_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(decl_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(field_id id)
{
    return isize(id);
}

} // namespace sgl::ast
