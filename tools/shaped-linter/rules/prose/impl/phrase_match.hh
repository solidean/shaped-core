#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-linter/prose/prose_view.hh>

// Matching stock phrases in prose: the shared machinery of the rules that flag a phrase, not a layout.
// A pattern is written as words, so a new variation of a phrase is one more string in its rule's list.

namespace scl::impl
{
/// One step of a phrase pattern.
struct phrase_step
{
    enum class step_kind : u8
    {
        /// One token among `alternatives`, consumed.
        token,
        /// The phrase starts a sentence or a clause: no word directly before it.
        sentence_start,
        /// The phrase ends a clause: nothing after it but punctuation or the end of the block.
        clause_end,
        /// The next token is none of `alternatives`; nothing is consumed.
        not_followed_by,
    };

    step_kind kind = step_kind::token;
    bool optional = false;
    cc::vector<cc::string> alternatives;
};

/// A compiled phrase pattern, matched against a block's words case-insensitively.
struct phrase_pattern
{
    cc::vector<phrase_step> steps;
};

/// Compiles the notation every phrase rule writes its patterns in, steps separated by spaces:
///
///   which|that     one of these words
///   [really|also]  optionally one of these words
///   ^              a sentence or clause starts here
///   $              a clause ends here
///   !set|cloud     the next word is none of these
///
/// Punctuation is a token of its own, and every dash (`-` between spaces, `--`, en and em dash) is spelled `—`.
/// A contraction is one word, with a typographic apostrophe read as `'`: `that's`.
phrase_pattern compile_phrase(cc::string_view notation);

/// The first match of any of `patterns` at each position of `block`, matches never overlapping.
/// The block's lines are read as one text, so a phrase wrapped across lines is still found; a span is clipped to the line it starts on.
/// Inline code is one opaque token, never a word of a phrase.
cc::vector<source_span> find_phrases(prose_block const& block, cc::span<phrase_pattern const> patterns);
} // namespace scl::impl
