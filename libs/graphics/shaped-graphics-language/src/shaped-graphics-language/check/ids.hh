#pragma once

#include <shaped-graphics-language/fwd.hh>

/// What the check pass produces is named by position, one id type per array, like the AST.
/// `none` is the absent link.

/// A position in `checked_module::types`.
enum class sgl::check::type_id : sgl::i32
{
    none = -1
};

/// A position in `checked_module::symbols`.
enum class sgl::check::symbol_id : sgl::i32
{
    none = -1
};

/// A position in `flat_entry_point::exprs`.
enum class sgl::check::flat_expr_id : sgl::i32
{
    none = -1
};

/// A position in `flat_entry_point::stmts`.
enum class sgl::check::flat_stmt_id : sgl::i32
{
    none = -1
};

/// A position in `flat_entry_point::locals`.
enum class sgl::check::local_id : sgl::i32
{
    none = -1
};

namespace sgl::check
{

[[nodiscard]] constexpr bool is_valid(type_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(symbol_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(flat_expr_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(flat_stmt_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(local_id id)
{
    return i32(id) >= 0;
}

/// The position an id names.
/// `id` must be valid.
[[nodiscard]] constexpr isize index_of(type_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(symbol_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(flat_expr_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(flat_stmt_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(local_id id)
{
    return isize(id);
}

} // namespace sgl::check
