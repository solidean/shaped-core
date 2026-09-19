#pragma once

#include <shaped-graphics-language/fwd.hh>

/// Every tree of a `parsed_file` links its nodes by id: the position of a node in the array that holds it.
/// One type per array, so a group id can never be handed to something that wants a form.
/// `none` is the absent link, and `parsed_file::at` is how an id becomes a node.

/// A position in `parsed_file::lines`.
enum class sgl::line_id : sgl::i32
{
    none = -1
};

/// A position in `parsed_file::tokens`.
/// A line's tokens are consecutive, which makes this the one id with a `next` and a `previous`.
enum class sgl::token_id : sgl::i32
{
    none = -1
};

/// A position in `parsed_file::groups`.
enum class sgl::group_id : sgl::i32
{
    none = -1
};

/// A position in `parsed_file::forms`.
enum class sgl::form_id : sgl::i32
{
    none = -1
};

namespace sgl
{

[[nodiscard]] constexpr bool is_valid(line_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(token_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(group_id id)
{
    return i32(id) >= 0;
}
[[nodiscard]] constexpr bool is_valid(form_id id)
{
    return i32(id) >= 0;
}

/// The position an id names, for a side array that runs parallel to the one the id belongs to.
/// `id` must be valid.
[[nodiscard]] constexpr isize index_of(line_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(token_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(group_id id)
{
    return isize(id);
}
[[nodiscard]] constexpr isize index_of(form_id id)
{
    return isize(id);
}

/// The token after `id` in source order, which may be one past the last token of its line or of the file.
/// `id` must be valid.
[[nodiscard]] constexpr token_id next(token_id id)
{
    return token_id(i32(id) + 1);
}

/// The token before `id` in source order.
/// `id` must be valid and must not be the first token of the file.
[[nodiscard]] constexpr token_id previous(token_id id)
{
    return token_id(i32(id) - 1);
}

} // namespace sgl
