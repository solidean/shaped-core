#include "format.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/ast/dump.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>

using namespace sgl;

namespace
{
[[nodiscard]] bool is_open(token_kind k)
{
    return k == token_kind::round_open || k == token_kind::square_open || k == token_kind::curly_open;
}

[[nodiscard]] bool is_close(token_kind k)
{
    return k == token_kind::round_close || k == token_kind::square_close || k == token_kind::curly_close;
}

[[nodiscard]] bool ends_operand(token_kind k)
{
    return k == token_kind::symbol || k == token_kind::wildcard || k == token_kind::quote_close || is_close(k);
}

[[nodiscard]] bool is_binary_operator(cc::string_view op)
{
    constexpr cc::string_view operators[] = {
        "=",  "==", "!=", "<", ">", "<=", ">=", "+",  "-",  "*",  "/",   "%",   "+=",  "-=",  "*=",
        "/=", "%=", "&",  "|", "^", "<<", ">>", "&=", "|=", "^=", "<<=", ">>=", "..<", "..=",
    };
    for (auto const o : operators)
        if (o == op)
            return true;
    return false;
}

/// An error that keeps a source from being formatted: what it means is not settled.
/// An operator that touches both operands has one reading, which the tree carries, and spacing it is the formatter's job.
[[nodiscard]] bool is_blocking(diagnostic const& d)
{
    return d.level != severity::warning && d.kind != diagnostic_kind::operator_needs_spaces;
}

/// The line, counted from 0, holding byte `offset`.
[[nodiscard]] i32 line_of(parsed_file const& f, u32 offset)
{
    auto line = 0;
    for (auto i = isize(0); i < f.lines.size() && f.lines[i].text.offset <= offset; ++i)
        line = i32(i);
    return line;
}

/// Every blocking syntax error of `f`, the phases' in order.
[[nodiscard]] cc::vector<diagnostic> blocking_errors(parsed_file const& f, ast::file_ast const& a)
{
    auto out = cc::vector<diagnostic>();
    for (auto const* list : {&f.diagnostics, &a.diagnostics})
        for (auto const& d : *list)
            if (is_blocking(d))
                out.push_back(d);
    return out;
}

/// The kinds of `errors`, which a formatted file must keep while the spans move.
[[nodiscard]] cc::vector<diagnostic_kind> kinds_of(cc::span<diagnostic const> errors)
{
    auto out = cc::vector<diagnostic_kind>();
    for (auto const& d : errors)
        out.push_back(d.kind);
    return out;
}

/// The first line past line `l`'s subtree in the line tree, whose lines are stored in pre-order.
[[nodiscard]] isize subtree_end(parsed_file const& f, isize l)
{
    for (auto up = line_id(i32(l)); is_valid(up); up = f.lines[index_of(up)].parent)
        if (is_valid(f.lines[index_of(up)].next_sibling))
            return index_of(f.lines[index_of(up)].next_sibling);
    return f.lines.size();
}

/// A line as the formatter writes it, with where its trailing comment stands.
struct formatted_line
{
    cc::string text;
    /// The end of the code before a trailing comment, and the comment's start; -1 for a line without both.
    isize code_end = -1;
    isize comment_start = -1;
};

/// The formatter over one parsed file: what each line becomes, and whether a set of lines formats safely.
struct formatter
{
    parsed_file const& file;
    cc::string reference_tree;
    cc::vector<diagnostic_kind> reference_errors;
    /// Binary operators that touch both neighbours, `a+b`, which take a space on each side.
    cc::vector<bool> spaced_operator;
    /// Whether each token stands inside a string, whose spacing is its content.
    cc::vector<bool> in_string;

    explicit formatter(parsed_file const& f, ast::file_ast const& a)
      : file(f), reference_tree(ast::dump(f, a)), reference_errors(kinds_of(blocking_errors(f, a)))
    {
        auto const& t = file.tokens;
        spaced_operator.resize_to_filled(t.size(), false);
        in_string.resize_to_filled(t.size(), false);
        auto depth = 0;
        for (auto i = isize(0); i < t.size(); ++i)
        {
            if (t[i].kind == token_kind::quote_close)
                --depth;
            in_string[i] = depth > 0;
            if (t[i].kind == token_kind::quote_open)
                ++depth;
        }
        for (auto i = isize(1); i + 1 < t.size(); ++i)
        {
            if (t[i].kind != token_kind::op || in_string[i] || !is_binary_operator(file.text_of(t[i].where)))
                continue;
            if (!fused(i) || !fused(i + 1) || !ends_operand(t[i - 1].kind))
                continue;
            // `1e-3` and `0x1p-3` are one number each, which the tokenizer split
            auto const before = file.text_of(t[i - 1].where);
            auto const op = file.text_of(t[i].where);
            if ((op == "-" || op == "+") && !before.empty() && before[0] >= '0' && before[0] <= '9'
                && (before.ends_with('e') || before.ends_with('E') || before.ends_with('p') || before.ends_with('P')))
                continue;
            spaced_operator[i] = true;
        }
    }

    /// Whether token `i` touches the one before it.
    [[nodiscard]] bool fused(isize i) const { return file.is_fused_left(token_id(i32(i))); }

    /// The whitespace between tokens `i - 1` and `i` of one line.
    [[nodiscard]] cc::string_view gap(isize i) const
    {
        auto const& t = file.tokens;
        auto const& a = t[i - 1];
        auto const& b = t[i];
        auto const written = file.text_of({.offset = a.where.end(), .length = b.where.offset - a.where.end()});
        if (in_string[i] || in_string[i - 1] || a.kind == token_kind::quote_open)
            return written;
        if (b.kind == token_kind::comment)
            return " "; // aligned with its neighbours' afterwards
        if (b.kind == token_kind::comma || b.kind == token_kind::semicolon || is_close(b.kind) || is_open(a.kind))
            return "";
        if (a.kind == token_kind::comma || a.kind == token_kind::semicolon)
            return " ";
        // `=>:` opens a block body, and keeps its colon
        if (a.kind == token_kind::double_arrow && b.kind == token_kind::colon)
            return written;
        // `name: type`, and a block's colon at its line's end
        if (b.kind == token_kind::colon)
            return "";
        if (a.kind == token_kind::colon)
            return " ";
        if (a.kind == token_kind::arrow || a.kind == token_kind::double_arrow || b.kind == token_kind::arrow
            || b.kind == token_kind::double_arrow)
            return " ";
        if (spaced_operator[i] || spaced_operator[i - 1])
            return " ";
        return written.empty() ? "" : " ";
    }

    /// Line `l` as the formatter writes it, without its terminator.
    [[nodiscard]] formatted_line format_line(isize l) const
    {
        auto const& line = file.lines[l];
        auto const written = file.text_of(line.text);
        if (line.kind == line_kind::blank)
            return {};
        if (line.kind == line_kind::string_content || line.token_count == 0)
            return {.text = cc::string(written)};
        auto result
            = formatted_line{.text = cc::string(written.subview({.offset = 0, .size = isize(line.indent_bytes)}))};
        auto& out = result.text;
        auto const first = isize(line.first_token);
        auto const last = first + isize(line.token_count);
        for (auto i = first; i < last; ++i)
        {
            auto const is_trailing_comment = i > first && file.tokens[i].kind == token_kind::comment && !in_string[i];
            if (is_trailing_comment)
                result.code_end = out.size();
            if (i > first)
                out += gap(i);
            if (is_trailing_comment)
                result.comment_start = out.size();
            out += file.text_of(file.tokens[i].where);
        }
        // what follows an opening quote at the line's end is the string's
        if (line.opens_string)
            out += file.text_of({.offset = file.tokens[last - 1].where.end(),
                                 .length = line.text.end() - file.tokens[last - 1].where.end()});
        else // a comment runs to the line's end, its trailing whitespace included
            while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
                out.resize_down_to(out.size() - 1);
        return result;
    }

    /// The file with the lines `formatted[l]` marks formatted and every other one as written.
    [[nodiscard]] cc::string render(cc::span<formatted_line const> lines, cc::span<bool const> formatted) const
    {
        auto out = cc::string();
        auto terminator = cc::string_view("\n");
        auto end = file.lines.size();
        // blank lines at the end go
        while (end > 0 && file.lines[end - 1].kind == line_kind::blank)
            --end;
        for (auto l = isize(0); l < end; ++l)
        {
            auto const& line = file.lines[l];
            out += formatted[l] ? cc::string_view(lines[l].text) : file.text_of(line.text);
            auto const t = file.text_of({.offset = line.text.end(), .length = line.terminator_length});
            if (!t.empty() && terminator == "\n")
                terminator = t;
            out += t.empty() ? terminator : t;
        }
        return out;
    }

    /// Whether `text` parses to the tree the source did.
    [[nodiscard]] bool parses_the_same(cc::string const& text) const
    {
        auto const f = parse(text);
        auto const a = ast::build(f);
        auto const errors = kinds_of(blocking_errors(f, a));
        if (errors.size() != reference_errors.size())
            return false;
        for (auto i = isize(0); i < errors.size(); ++i)
            if (errors[i] != reference_errors[i])
                return false;
        return ast::dump(f, a) == reference_tree;
    }
};
} // namespace

namespace
{
/// The trailing comments of consecutive lines, lined up one space past the longest code among them, a run breaking where
/// a line would move its comment more than the limit; a line of its own takes one space.
void align_comments(cc::span<formatted_line> lines, cc::span<bool const> formatted)
{
    auto run = cc::vector<isize>();
    auto lowest = isize(0);
    auto highest = isize(0);
    auto const flush = [&]
    {
        for (auto const l : run)
        {
            auto& line = lines[l];
            auto const target = run.size() > 1 ? highest + 1 : line.code_end + 1;
            auto aligned = cc::string(cc::string_view(line.text).subview({.offset = 0, .size = line.code_end}));
            for (auto c = line.code_end; c < target; ++c)
                aligned += " ";
            auto const comment_start = aligned.size();
            aligned += cc::string_view(line.text).subview(line.comment_start);
            line.text = cc::move(aligned);
            line.comment_start = comment_start;
        }
        run.clear();
    };
    for (auto l = isize(0); l < lines.size(); ++l)
    {
        auto const end = lines[l].code_end;
        if (end < 0 || !formatted[l])
        {
            flush();
            continue;
        }
        auto const is_close = !run.empty() && run.back() == l - 1
                           && cc::max(highest, end) - cc::min(lowest, end) <= max_comment_alignment_shift;
        if (!is_close)
        {
            flush();
            lowest = end;
            highest = end;
        }
        lowest = cc::min(lowest, end);
        highest = cc::max(highest, end);
        run.push_back(l);
    }
    flush();
}

/// The lines each syntax error leaves as written: the children of its line's parent, its block and the block's siblings;
/// at the top level, its own declaration.
[[nodiscard]] cc::vector<format_skip> skips_of(parsed_file const& f, cc::span<diagnostic const> errors)
{
    auto out = cc::vector<format_skip>();
    for (auto const& d : errors)
    {
        auto const l = isize(line_of(f, d.where.offset));
        auto first = l;
        auto end = subtree_end(f, l);
        if (auto const parent = f.lines[l].parent; is_valid(parent))
        {
            first = index_of(f.lines[index_of(parent)].first_child);
            end = subtree_end(f, index_of(parent));
        }
        out.push_back({.first_line = i32(first),
                       .last_line = i32(end - 1),
                       .error_line = i32(l),
                       .error = cc::string(summary_of(d.kind))});
    }
    // in source order, one run where two overlap, named after its first error
    for (auto i = isize(1); i < out.size(); ++i)
        for (auto j = i; j > 0 && out[j].first_line < out[j - 1].first_line; --j)
            cc::swap(out[j], out[j - 1]);
    auto merged = cc::vector<format_skip>();
    for (auto& s : out)
        if (!merged.empty() && s.first_line <= merged.back().last_line + 1)
            merged.back().last_line = cc::max(merged.back().last_line, s.last_line);
        else
            merged.push_back(cc::move(s));
    return merged;
}
} // namespace

formatted_source sgl::format_source(cc::string_view source)
{
    auto const f = parse(cc::string(source));
    auto const a = ast::build(f);
    auto out = formatted_source{.skipped = skips_of(f, blocking_errors(f, a))};
    auto is_skipped = cc::vector<bool>();
    is_skipped.resize_to_filled(f.lines.size(), false);
    for (auto const& s : out.skipped)
        for (auto l = s.first_line; l <= s.last_line; ++l)
            is_skipped[l] = true;

    auto const fm = formatter(f, a);
    auto lines = cc::vector<formatted_line>();
    for (auto l = isize(0); l < f.lines.size(); ++l)
        lines.push_back(fm.format_line(l));
    auto formatted = cc::vector<bool>();
    for (auto l = isize(0); l < f.lines.size(); ++l)
        formatted.push_back(!is_skipped[l]);
    align_comments(lines, formatted);
    out.text = fm.render(lines, formatted);
    if (fm.parses_the_same(out.text))
        return out;

    // some rule is wrong for some line: take the changed lines one at a time, each only where the file still parses the
    // same with it
    for (auto l = isize(0); l < f.lines.size(); ++l)
        formatted[l]
            = !is_skipped[l] && (lines[l].text == f.text_of(f.lines[l].text) || f.lines[l].kind == line_kind::blank);
    for (auto l = isize(0); l < f.lines.size(); ++l)
    {
        if (formatted[l] || is_skipped[l])
            continue;
        formatted[l] = true;
        if (!fm.parses_the_same(fm.render(lines, formatted)))
        {
            formatted[l] = false;
            out.unformatted_lines.push_back(i32(l));
        }
    }
    out.text = fm.render(lines, formatted);
    return out;
}
