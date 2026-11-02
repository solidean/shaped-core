#include "phrase_match.hh"

#include <clean-core/string/char_predicates.hh>

namespace scl::impl
{
namespace
{
enum class token_kind : u8
{
    word,
    punctuation,
    code,
};

struct phrase_token
{
    token_kind kind = token_kind::word;
    cc::string text; // lowercased, apostrophes and dashes normalized
    isize begin = 0; // into the joined text
    isize end = 0;
};

/// A block's lines as one text, with where each byte of it came from.
struct joined_text
{
    cc::string text;
    cc::vector<u32> source_offset; // per byte of `text`
    cc::vector<i32> line_of;       // per byte of `text`, -1 for the space joining two lines
};

joined_text join(prose_block const& block)
{
    auto j = joined_text{};
    for (isize l = 0; l < block.lines.size(); ++l)
    {
        auto const& line = block.lines[l];
        for (isize i = 0; i < line.text.size(); ++i)
        {
            j.text.push_back(line.text[i]);
            j.source_offset.push_back(line.span.byte_begin + u32(i));
            j.line_of.push_back(i32(l));
        }
        j.text.push_back(' ');
        j.source_offset.push_back(line.span.byte_begin + u32(line.text.size()));
        j.line_of.push_back(-1);
    }
    return j;
}

/// The length of a UTF-8 dash or right single quote at `i`, or 0.
isize dash_at(cc::string_view s, isize i)
{
    // U+2013 en dash and U+2014 em dash
    if (i + 2 < s.size() && u8(s[i]) == 0xE2 && u8(s[i + 1]) == 0x80 && (u8(s[i + 2]) == 0x93 || u8(s[i + 2]) == 0x94))
        return 3;
    if (i + 1 < s.size() && s[i] == '-' && s[i + 1] == '-')
        return 2;
    return 0;
}

isize apostrophe_at(cc::string_view s, isize i)
{
    if (i < s.size() && s[i] == '\'')
        return 1;
    // U+2019 right single quotation mark
    if (i + 2 < s.size() && u8(s[i]) == 0xE2 && u8(s[i + 1]) == 0x80 && u8(s[i + 2]) == 0x99)
        return 3;
    return 0;
}

bool is_word_byte(cc::string_view s, isize i)
{
    auto const c = s[i];
    if (cc::is_alphanumeric(c) || c == '_')
        return true;
    // Any other non-ASCII byte is a letter of some script, unless it opens a dash or a quote.
    return u8(c) >= 0x80 && dash_at(s, i) == 0 && apostrophe_at(s, i) == 0;
}

cc::vector<phrase_token> tokenize(cc::string_view s)
{
    auto tokens = cc::vector<phrase_token>{};
    isize i = 0;
    while (i < s.size())
    {
        auto const c = s[i];
        if (c == ' ' || c == '\t')
        {
            ++i;
            continue;
        }

        auto const begin = i;
        if (c == '`')
        {
            ++i;
            while (i < s.size() && s[i] != '`')
                ++i;
            i = i < s.size() ? i + 1 : i;
            tokens.push_back({.kind = token_kind::code, .text = cc::string("`"), .begin = begin, .end = i});
            continue;
        }

        if (auto const n = dash_at(s, i); n > 0)
        {
            i += n;
            tokens.push_back({.kind = token_kind::punctuation, .text = cc::string("—"), .begin = begin, .end = i});
            continue;
        }

        if (is_word_byte(s, i))
        {
            auto text = cc::string{};
            while (i < s.size())
            {
                if (is_word_byte(s, i))
                {
                    text.push_back(cc::to_lower(s[i]));
                    ++i;
                    continue;
                }
                // An apostrophe or a hyphen inside a word keeps it one word: `that's`, `well-known`.
                auto const inner = s[i] == '-' ? 1 : apostrophe_at(s, i);
                if (inner > 0 && i + inner < s.size() && is_word_byte(s, i + inner))
                {
                    text.push_back(s[i] == '-' ? '-' : '\'');
                    i += inner;
                    continue;
                }
                break;
            }
            tokens.push_back({.kind = token_kind::word, .text = cc::move(text), .begin = begin, .end = i});
            continue;
        }

        // A hyphen standing between spaces is a dash.
        auto const lone_hyphen = c == '-' && (i == 0 || s[i - 1] == ' ') && (i + 1 >= s.size() || s[i + 1] == ' ');
        ++i;
        tokens.push_back({.kind = token_kind::punctuation,
                          .text = lone_hyphen ? cc::string("—") : cc::string(s.subview({.start = begin, .end = i})),
                          .begin = begin,
                          .end = i});
    }
    return tokens;
}

bool is_one_of(phrase_token const& t, phrase_step const& step)
{
    if (t.kind == token_kind::code)
        return false;
    for (auto const& a : step.alternatives)
        if (t.text == a)
            return true;
    return false;
}

/// Punctuation that can close a clause; a closing parenthesis is not, since `(… is the point)` is too often literal.
bool closes_clause(phrase_token const& t)
{
    if (t.kind != token_kind::punctuation)
        return false;
    for (auto const p : {".", ",", ";", ":", "!", "?", "—", "*", "\"", "'"})
        if (t.text == p)
            return true;
    return false;
}

bool match_from(cc::span<phrase_step const> steps, isize si, cc::span<phrase_token const> tokens, isize ti, isize& end)
{
    if (si == steps.size())
    {
        end = ti;
        return true;
    }

    auto const& step = steps[si];
    switch (step.kind)
    {
    case phrase_step::step_kind::sentence_start:
    {
        auto const starts = ti == 0 || (tokens[ti - 1].kind == token_kind::punctuation && tokens[ti - 1].text != ",");
        return starts && match_from(steps, si + 1, tokens, ti, end);
    }
    case phrase_step::step_kind::clause_end:
        return (ti == tokens.size() || closes_clause(tokens[ti])) && match_from(steps, si + 1, tokens, ti, end);
    case phrase_step::step_kind::not_followed_by:
        return !(ti < tokens.size() && is_one_of(tokens[ti], step)) && match_from(steps, si + 1, tokens, ti, end);
    case phrase_step::step_kind::token:
        if (ti < tokens.size() && is_one_of(tokens[ti], step) && match_from(steps, si + 1, tokens, ti + 1, end))
            return true;
        return step.optional && match_from(steps, si + 1, tokens, ti, end);
    }
    return false;
}

cc::vector<cc::string> split_alternatives(cc::string_view s)
{
    auto result = cc::vector<cc::string>{};
    isize begin = 0;
    for (isize i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '|')
        {
            result.push_back(cc::string(s.subview({.start = begin, .end = i})));
            begin = i + 1;
        }
    return result;
}
} // namespace

phrase_pattern compile_phrase(cc::string_view notation)
{
    auto pattern = phrase_pattern{};
    isize i = 0;
    while (i < notation.size())
    {
        if (notation[i] == ' ')
        {
            ++i;
            continue;
        }
        auto const begin = i;
        while (i < notation.size() && notation[i] != ' ')
            ++i;
        auto const word = notation.subview({.start = begin, .end = i});

        if (word == "^")
            pattern.steps.push_back({.kind = phrase_step::step_kind::sentence_start});
        else if (word == "$")
            pattern.steps.push_back({.kind = phrase_step::step_kind::clause_end});
        else if (word[0] == '!')
            pattern.steps.push_back({.kind = phrase_step::step_kind::not_followed_by,
                                     .alternatives = split_alternatives(word.subview({.start = 1, .end = word.size()}))});
        else if (word[0] == '[' && word[word.size() - 1] == ']')
            pattern.steps.push_back(
                {.kind = phrase_step::step_kind::token,
                 .optional = true,
                 .alternatives = split_alternatives(word.subview({.start = 1, .end = word.size() - 1}))});
        else
            pattern.steps.push_back({.kind = phrase_step::step_kind::token, .alternatives = split_alternatives(word)});
    }
    return pattern;
}

cc::vector<source_span> find_phrases(prose_block const& block, cc::span<phrase_pattern const> patterns)
{
    auto found = cc::vector<source_span>{};
    if (block.lines.empty())
        return found;

    auto const joined = join(block);
    auto const tokens = tokenize(joined.text);
    auto const file_id = block.lines[0].span.file_id;

    isize ti = 0;
    while (ti < tokens.size())
    {
        auto matched_end = isize(-1);
        for (auto const& pattern : patterns)
        {
            auto end = isize(0);
            if (match_from(pattern.steps, 0, tokens, ti, end) && end > ti)
            {
                matched_end = end;
                break;
            }
        }
        if (matched_end < 0)
        {
            ++ti;
            continue;
        }

        // The carets stay on the line the phrase starts on.
        auto const first = tokens[ti].begin;
        auto last = tokens[matched_end - 1].end - 1;
        while (joined.line_of[last] != joined.line_of[first])
            --last;
        found.push_back(
            {.file_id = file_id, .byte_begin = joined.source_offset[first], .byte_end = joined.source_offset[last] + 1});
        ti = matched_end;
    }
    return found;
}
} // namespace scl::impl
