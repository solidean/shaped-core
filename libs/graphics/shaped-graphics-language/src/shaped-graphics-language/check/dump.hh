#pragma once

#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/checked_module.hh>

namespace sgl::check
{
/// The checked module as s-expressions: every symbol in declaration order, then every flat entry point.
///
///     (struct float3 builtin (x : float) (y : float) (z : float))
///     (fun scale_color builtin operator:* (c : float3) (s : float) -> float3)
///     (binding constants inline (view_projection : mat4))
///     (entry pixel main_ps (p : pixel_input) -> target
///       (let n : vec3 = (call normalize (member (local p : pixel_input) normal : vec3) : vec3))
///       (return (construct (local n : vec3) : target)))
///
/// - A symbol is its kind, its name, what its attributes made of it, and its members or its signature.
///   `failed` follows the name of a symbol nothing can rely on, and nothing else is printed for it.
/// - A field that carries `@position` is written `name{@position}`.
/// - A flat expression ends in ` : type`, and a local is written with its minted name.
/// - A flat statement has a line of its own.
///
/// For tests and for reading by eye; the format is not stable and must not be parsed.
[[nodiscard]] cc::string dump(checked_module const& m);

/// Only the flat entry points, in the same format.
[[nodiscard]] cc::string dump_entry_points(checked_module const& m);

/// One line per diagnostic, in the order they were reported: `unknown-name @1:120+4 foo`.
/// The number before the colon is the file, 0 for the prelude; the detail follows when there is one.
[[nodiscard]] cc::string dump_diagnostics(checked_module const& m);
} // namespace sgl::check
