#include "verify.hh"

#include <clean-core/string/format.hh>
#include <shaped-linter/lex/comment_lexer.hh>
#include <shaped-linter/lex/markdown_scanner.hh>
#include <shaped-linter/lex/source_buffer.hh>
#include <shaped-linter/prose/prose_view.hh>

namespace scl
{
namespace
{
/// A word of prose and the line it sits on.
struct prose_word
{
    cc::string text;
    u32 line = 0;
};

/// A token that only structures a line: a blockquote `>`, a list or heading marker, a table pipe, an ordered-list number.
bool is_marker(cc::string_view w)
{
    auto only_marks = true;
    for (auto const c : w)
        if (c != '#' && c != '>' && c != '*' && c != '+' && c != '-' && c != '|')
            only_marks = false;
    if (only_marks)
        return true;

    // `1.` or `12)`: digits and one closing mark.
    if (w.size() < 2 || (w[w.size() - 1] != '.' && w[w.size() - 1] != ')'))
        return false;
    for (isize i = 0; i + 1 < w.size(); ++i)
        if (w[i] < '0' || w[i] > '9')
            return false;
    return true;
}

/// The words of a file's prose, in order, with a word split at a hyphen across a line break rejoined.
cc::vector<prose_word> prose_words(source_buffer const& buffer, source_language language, token_stream const& tokens)
{
    auto words = cc::vector<prose_word>{};
    for (auto const& block : extract_prose(buffer, language, tokens).blocks)
        for (auto const& line : block.lines)
        {
            auto const at = buffer.line_col_at(line.span.byte_begin).line;
            auto const text = line.text;
            for (isize i = 0; i < text.size();)
            {
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
                    ++i;
                auto const begin = i;
                while (i < text.size() && text[i] != ' ' && text[i] != '\t')
                    ++i;
                if (i > begin && !is_marker(text.subview({.start = begin, .end = i})))
                    words.push_back({.text = cc::string(text.subview({.start = begin, .end = i})), .line = at});
            }
        }

    auto joined = cc::vector<prose_word>{};
    for (isize i = 0; i < words.size(); ++i)
    {
        auto w = words[i];
        // `cross-` at a line's end and `check` at the next line's start are one word, as a reader reads them.
        while (w.text.size() > 1 && w.text[w.text.size() - 1] == '-' && i + 1 < words.size() && words[i + 1].line != w.line)
        {
            w.text += words[i + 1].text;
            ++i;
        }
        joined.push_back(cc::move(w));
    }
    return joined;
}

/// A few words around `i`, for a message that says where the prose diverged.
cc::string excerpt(cc::span<prose_word const> words, isize i)
{
    auto out = cc::string{};
    auto const begin = i > 3 ? i - 3 : isize(0);
    auto const end = i + 4 < words.size() ? i + 4 : words.size();
    for (auto k = begin; k < end; ++k)
    {
        if (!out.empty())
            out += " ";
        out += words[k].text;
    }
    return out;
}

/// The lines of a markdown file that are not prose, trimmed: fences, fenced code, tables, frontmatter.
cc::vector<prose_word> markdown_structure(source_buffer const& buffer)
{
    auto out = cc::vector<prose_word>{};
    for (auto const& line : scan_markdown(buffer))
    {
        if (line.kind == markdown_line_kind::text || line.kind == markdown_line_kind::blank)
            continue;
        auto text = buffer.span_text(line.span);
        while (!text.empty() && (text[0] == ' ' || text[0] == '\t'))
            text = text.subview({.start = 1, .end = text.size()});
        while (!text.empty() && (text[text.size() - 1] == ' ' || text[text.size() - 1] == '\t' || text[text.size() - 1] == '\r'))
            text = text.subview({.start = 0, .end = text.size() - 1});
        out.push_back({.text = cc::string(text), .line = buffer.line_col_at(line.span.byte_begin).line});
    }
    return out;
}

/// The first place two sequences differ, or -1 when they are equal.
template <class T, class Same>
isize first_difference(cc::span<T const> a, cc::span<T const> b, Same same)
{
    auto const shared = a.size() < b.size() ? a.size() : b.size();
    for (isize i = 0; i < shared; ++i)
        if (!same(a[i], b[i]))
            return i;
    return a.size() == b.size() ? -1 : shared;
}
} // namespace

cc::string_view comparable_text(token const& t)
{
    if (t.kind != token_kind::preprocessor_directive)
        return t.text;

    auto end = t.text.size();
    while (end > 0 && (t.text[end - 1] == ' ' || t.text[end - 1] == '\t'))
        --end;
    return t.text.subview({.start = 0, .end = end});
}

cc::vector<token> significant_tokens(token_stream const& ts, source_language language)
{
    cc::vector<token> out;
    auto const all = ts.all();
    for (isize i = 0; i < all.size(); ++i)
    {
        if (all[i].is_trivia())
            continue;
        if (language == source_language::python && is_python_docstring(all, i))
            continue;
        out.push_back(all[i]);
    }
    return out;
}

prose_edit_verdict verify_prose_edit(cc::string_view path, cc::string_view before, cc::string_view after)
{
    auto verdict = prose_edit_verdict{};
    auto const language = language_from_path(path);
    auto const old_buffer = source_buffer::from_text(cc::string(before), path, 0);
    auto const new_buffer = source_buffer::from_text(cc::string(after), path, 0);

    auto old_tokens = lex_for(old_buffer, language);
    auto new_tokens = lex_for(new_buffer, language);
    if (old_tokens.has_error() || new_tokens.has_error())
    {
        verdict.unreadable = true;
        return verdict;
    }

    if (language == source_language::markdown)
    {
        auto const a = markdown_structure(old_buffer);
        auto const b = markdown_structure(new_buffer);
        auto const at = first_difference<prose_word>(a, b, [](prose_word const& x, prose_word const& y) { return x.text == y.text; });
        if (at >= 0)
            verdict.code_change
                = cc::format("line {}: '{}' where '{}' was", at < b.size() ? b[at].line : new_buffer.line_count(),
                             at < b.size() ? cc::string_view(b[at].text) : "<nothing>", at < a.size() ? cc::string_view(a[at].text) : "<nothing>");
    }
    else
    {
        auto const a = significant_tokens(old_tokens.value(), language);
        auto const b = significant_tokens(new_tokens.value(), language);
        auto const at = first_difference<token>(a, b, [](token const& x, token const& y)
                                                { return x.kind == y.kind && comparable_text(x) == comparable_text(y); });
        if (at >= 0)
            verdict.code_change = cc::format(
                "line {}: '{}' where '{}' was",
                at < b.size() ? new_buffer.line_col_at(b[at].span.byte_begin).line : new_buffer.line_count(),
                at < b.size() ? comparable_text(b[at]) : cc::string_view("<nothing>"),
                at < a.size() ? comparable_text(a[at]) : cc::string_view("<nothing>"));
    }

    auto const a = prose_words(old_buffer, language, old_tokens.value());
    auto const b = prose_words(new_buffer, language, new_tokens.value());
    auto const at = first_difference<prose_word>(a, b, [](prose_word const& x, prose_word const& y) { return x.text == y.text; });
    if (at >= 0)
        verdict.word_change = cc::format("line {}: '{}' where '{}' was", at < b.size() ? b[at].line : new_buffer.line_count(),
                                         excerpt(b, at), excerpt(a, at));
    return verdict;
}
} // namespace scl
