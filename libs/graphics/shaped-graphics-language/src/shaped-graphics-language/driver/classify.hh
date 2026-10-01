#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>

/// What a token of the source is, in SGL's own words: what an editor colours, and what a highlighter is checked against.
/// Without a checked module the syntax and the file's own declarations decide: a declared name has its class, and every use
/// of a name is `name`.
enum class sgl::token_class : sgl::u8
{
    keyword,
    /// A keyword or word operator that steers control flow, such as `if`, `return` and `and`.
    control,
    number,
    string,
    comment,
    /// A `///` comment.
    doc_comment,
    /// An operator, a word operator such as `and` included, and the `$` of an interpolation.
    op,
    /// `@name`, the `@` included.
    attribute,

    struct_,
    enum_,
    /// Any other type: a `type` alias, a type parameter, `void`.
    type,
    function,
    method,
    property,
    field,
    binding,
    binding_member,
    enum_case,
    constant,
    pipeline,
    parameter,
    /// The name a named argument or an object element gives: `center` in `f(center = 1.0)` and in `{center = 1.0}`.
    argument,
    local,
    mutable_local,
    /// `self`, as a parameter and as a use.
    self_,
    /// A name the check did not resolve; without a check, every use of a name.
    name,
};

/// One classified span, never overlapping another; a number fused from several tokens is one span.
struct sgl::classified_span
{
    source_span where;
    token_class cls = token_class::name;
    /// The place the thing is declared, rather than a use of it.
    bool is_declaration = false;
    /// Declared in the prelude: a builtin type, a builtin function, `true`.
    bool is_from_prelude = false;
    /// A call of a builtin whose record `constrains_control_flow`: a derivative, a barrier, an atomic.
    /// Only a checked module knows it, since an atomic's name is also an ordinary function's.
    bool constrains_control_flow = false;

    constexpr bool operator==(classified_span const&) const = default;
};

struct sgl::classify_options
{
    /// The module `file` was checked in; null classifies by the syntax and the file's declarations alone.
    check::checked_module const* module = nullptr;
    /// `file`'s position among the files the module was checked from; required with `module`.
    i32 file_index = -1;
};

namespace sgl
{
/// Every token of `file` that has a class, in source order.
/// A token without one, such as a bracket or the wildcard `_`, is left out.
[[nodiscard]] cc::vector<classified_span> classify(parsed_file const& file,
                                                   ast::file_ast const& ast,
                                                   classify_options const& options = {});

[[nodiscard]] cc::string_view to_string(token_class c);
} // namespace sgl
