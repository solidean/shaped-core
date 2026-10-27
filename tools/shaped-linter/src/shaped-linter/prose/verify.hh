#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-linter/fwd.hh>
#include <shaped-linter/lex/source_language.hh>
#include <shaped-linter/lex/token_stream.hh>

namespace scl
{
struct prose_edit_verdict;
} // namespace scl

/// How one file changed between two versions, judged as an edit that should have touched prose alone.
/// Each field is empty when that half is unchanged, and otherwise names the first difference, with its line in the newer version.
struct scl::prose_edit_verdict
{
    /// Code moved: a token outside every comment and docstring, or a fenced block, table or frontmatter line of markdown.
    cc::string code_change;

    /// The words of the prose moved: one added, dropped, or spelled differently.
    /// Line breaks, indentation, comment markers, blockquote `>`, list markers and a word hyphenated across a line break do not count.
    cc::string word_change;

    /// Either version would not lex, so neither half could be judged.
    bool unreadable = false;
};

namespace scl
{
/// The tokens a prose edit must leave untouched: every token but trivia, a Python docstring aside, since every prose rule reads it as prose.
cc::vector<token> significant_tokens(token_stream const& ts, source_language language);

/// The spelling two token sequences are compared by.
/// A preprocessor directive is lexed opaquely up to a trailing `//`, so its text swallows the spaces before that comment; they are trimmed here,
/// which lets an edit re-align a trailing comment while the directive itself still has to match byte for byte.
cc::string_view comparable_text(token const& t);

/// Judge `after` against `before`, two versions of the file at `path`, whose extension picks the language.
prose_edit_verdict verify_prose_edit(cc::string_view path, cc::string_view before, cc::string_view after);
} // namespace scl
