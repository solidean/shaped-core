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
} // namespace sgl
