#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/driver/classify.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/source/source_span.hh>

/// What a name of the source refers to, as the check resolved it: what an editor shows on hover and jumps to.
struct sgl::reference
{
    /// The name asked about, in the file asked about.
    source_span where;
    token_class cls = token_class::name;
    /// The file that declares it, a position among the module's files; -1 where nothing does, as for a swizzle.
    i32 declaring_file = -1;
    /// The declared name in `declaring_file`.
    source_span declared;
    /// The declaration as SGL would write it, `fun dot(a: vec3, b: vec3) -> float` or `let d : float`.
    cc::string signature;
    /// The `///` lines above the declaration, each without its slashes, joined by newlines; empty without any.
    cc::string doc;
    /// For a function, the other functions of its overload set, each as `signature` writes it.
    cc::vector<cc::string> overloads;
};

/// One place a name stands for what another place's name refers to: the declaration, or a use.
struct sgl::occurrence
{
    /// A position among the module's files.
    i32 file = -1;
    source_span where;
    bool is_declaration = false;

    bool operator==(occurrence const&) const = default;
};

/// Where a parameter stands in `call_signature::label`, in bytes.
struct sgl::parameter_label
{
    u32 offset = 0;
    u32 length = 0;

    [[nodiscard]] constexpr u32 end() const { return offset + length; }

    bool operator==(parameter_label const&) const = default;
};

/// One function a call may resolve to, as the call reads it: `dot(a: vec3, b: vec3) -> float`, with no receiver.
struct sgl::call_signature
{
    cc::string label;
    cc::vector<parameter_label> parameters;
    cc::string doc;
};

/// What an editor shows while a call's arguments are written: the call's overload set, and where the cursor is in it.
struct sgl::signature_help
{
    cc::vector<call_signature> signatures;
    /// The one the call resolved to while it has a parameter at `active_parameter`; else the first that does.
    i32 active_signature = 0;
    /// The position of the argument the cursor is in, or the parameter that argument names.
    /// May lie past the active signature's parameters, as a surplus argument does.
    i32 active_parameter = 0;
};

/// A name an editor may complete at a position, and what it is.
struct sgl::completion
{
    cc::string label;
    token_class cls = token_class::name;
    /// Its type or signature, as `reference::signature` writes it; empty for a keyword.
    cc::string detail;

    bool operator==(completion const&) const = default;
};

namespace sgl
{
/// What the name at byte `offset` of file `file_index` refers to; nothing where no resolved name, use or declaration
/// stands there.
/// An offset right after a name's last byte still names it, as a cursor at a word's end does.
/// `files` are the files `m` was checked from, in its order.
[[nodiscard]] cc::optional<reference> reference_at(cc::span<check::module_file const> files,
                                                   check::checked_module const& m,
                                                   i32 file_index,
                                                   u32 offset);

/// What may be written at byte `offset` of file `file_index`, the word that ends there being a prefix of each.
/// After `x.` it is what `x` has: a struct's fields, methods and properties, a binding's members, an enum's cases.
/// Anywhere else it is the locals and parameters in scope, the module's and the prelude's names, and the keywords.
/// A name declared twice, as an overloaded function is, is one completion.
[[nodiscard]] cc::vector<completion> completions_at(cc::span<check::module_file const> files,
                                                    check::checked_module const& m,
                                                    i32 file_index,
                                                    u32 offset);

/// Every occurrence of what the name at byte `offset` of file `file_index` refers to, its declaration included, in file
/// and then source order; empty where the name refers to nothing declared, as a swizzle or `self`.
/// A function's occurrences are its own: another overload of its name is something else.
[[nodiscard]] cc::vector<occurrence> occurrences_at(cc::span<check::module_file const> files,
                                                    check::checked_module const& m,
                                                    i32 file_index,
                                                    u32 offset);

/// The call whose parentheses hold byte `offset` of file `file_index`, the innermost one; nothing outside any call.
/// It reads the tokens rather than the check, so it answers while the call is half written and does not parse.
[[nodiscard]] cc::optional<signature_help> signature_help_at(cc::span<check::module_file const> files,
                                                             check::checked_module const& m,
                                                             i32 file_index,
                                                             u32 offset);

/// Whether `word` is one of SGL's keywords, which no declaration may take as its name.
[[nodiscard]] bool is_keyword(cc::string_view word);
} // namespace sgl
