#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

namespace sgl
{
/// Reads a grouped file into its form tree: `file.root_form` and everything under it.
///
/// `keywords` is the only thing the parser knows about the surface language.
/// A form that starts with one is a keyword form, which takes whole comma-separated expressions; every other name
/// applies tightly, so `return a + b` and `f a + g b` both read the way they look.
/// The word operators `and`, `or`, `not`, `as` and `in` are part of the form tree itself and need not be listed.
///
/// Loosest first: `;`, assignment, `=>`, keyword form, `and or not`, comparisons, `: -> as in`, ranges, bit-like,
/// add-like, mul-like, application, prefix and postfix operators, fused lists and members.
///
/// Must be called once, on a grouped file.
void parse_forms(parsed_file& file, cc::span<cc::string_view const> keywords);

/// SGL's keywords, as listed in libs/graphics/shaped-graphics-language/docs/spec/keywords.md.
[[nodiscard]] cc::span<cc::string_view const> default_keywords();
} // namespace sgl
