#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>

/// What a declaration of the outline is, as an editor's outline shows it.
enum class sgl::outline_kind : sgl::u8
{
    function,
    method,
    property,
    structure,
    enumeration,
    enum_case,
    field,
    binding,
    constant,
    sampler,
    pipeline,
    type,
    test,
    module,
};

/// One declaration of a file, with the ones it holds: a struct's fields, methods and properties, an enum's cases and a
/// binding's members.
struct sgl::outline_entry
{
    cc::string name;
    outline_kind kind = outline_kind::function;
    /// The whole declaration, its block included.
    source_span where;
    /// The name within `where`; the `test` keyword for a test, which has no name.
    source_span name_span;
    /// The declaration's first line as written, `fun shade(n: vec3) -> vec3`, without a trailing block colon.
    cc::string detail;
    cc::vector<outline_entry> children;
};

/// A run of lines an editor may fold to its first.
struct sgl::fold_range
{
    /// Counted from 0; `last_line` > `first_line`.
    u32 first_line = 0;
    u32 last_line = 0;
    /// A run of comment lines, which an editor may fold on its own.
    bool is_comment = false;

    bool operator==(fold_range const&) const = default;
};

namespace sgl
{
/// The declarations of `ast` in source order, each with what it holds; read from the tree alone, so a file whose check
/// fails still has its outline.
/// A `use`, a `require`, a notation and a declaration that did not parse have no entry.
[[nodiscard]] cc::vector<outline_entry> outline_of(parsed_file const& file, ast::file_ast const& ast);

/// Every line that owns deeper lines folds to itself, through its last descendant that is not blank.
/// So does a run of two or more `//` lines, as a comment.
[[nodiscard]] cc::vector<fold_range> folds_of(parsed_file const& file);
} // namespace sgl
