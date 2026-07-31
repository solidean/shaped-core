#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>

namespace scl
{
/// What a file is written in, decided from its extension alone — the engine's dispatch key.
/// Each language has its own front end, and a rule declares which languages it applies to.
enum class source_language : u8
{
    cpp,
    python,
    markdown,
};

/// The language of `path`, from its extension.
/// Anything unrecognized — including the `<memory>` of an
/// in-memory buffer — is C++, which keeps every existing rule and test unchanged.
source_language language_from_path(cc::string_view path);

/// A short human-readable name, for test diagnostics.
cc::string_view source_language_name(source_language l);

/// A set of languages, packed into the bits of a `u8`. `rule::languages` is one of these.
constexpr u8 language_bit(source_language l)
{
    return u8(1u << u32(l));
}

constexpr u8 k_cpp_only = language_bit(source_language::cpp);
constexpr u8 k_all_languages = language_bit(source_language::cpp) | language_bit(source_language::python)
                             | language_bit(source_language::markdown);
} // namespace scl
