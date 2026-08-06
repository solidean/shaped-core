#include "plan.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>

namespace scl
{
namespace
{
/// The next line of `text` starting at `pos`, and where the line after it begins.
/// A trailing `\r` is dropped, so a CRLF plan reads the same as an LF one.
struct next_line_result
{
    cc::string_view line;
    isize next_pos = 0;
};
next_line_result next_line(cc::string_view text, isize pos)
{
    auto end = pos;
    while (end < text.size() && text[end] != '\n')
        ++end;

    auto content_end = end;
    if (content_end > pos && text[content_end - 1] == '\r')
        --content_end;

    return {.line = text.subview({.start = pos, .end = content_end}), .next_pos = end < text.size() ? end + 1 : end};
}

bool is_blank(cc::string_view line)
{
    for (auto const c : line)
        if (c != ' ' && c != '\t')
            return false;
    return true;
}

cc::string_view trim(cc::string_view s)
{
    isize begin = 0;
    while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t'))
        ++begin;
    auto end = s.size();
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t'))
        --end;
    return s.subview({.start = begin, .end = end});
}

/// A 1-based line number: digits only, at least one, and never 0.
cc::result<u32> parse_line_number(cc::string_view s)
{
    if (s.empty())
        return cc::error("expected a line number");

    u64 value = 0;
    for (auto const c : s)
    {
        if (c < '0' || c > '9')
            return cc::error(cc::format("'{}' is not a line number", s));
        value = value * 10 + u64(c - '0');
        if (value > 0xFFFF'FFFF)
            return cc::error(cc::format("line number '{}' is out of range", s));
    }
    if (value == 0)
        return cc::error("line numbers are 1-based, 0 is not a line");

    return u32(value);
}

/// Parse the `[…]` span header into an edit with no lines yet.
cc::result<plan_edit> parse_span_header(cc::string_view line)
{
    auto const body = trim(line.subview({.start = 1, .end = line.size() - 1}));

    if (body.starts_with("+"))
    {
        auto at = parse_line_number(trim(body.subview(1)));
        CC_RETURN_IF_ERROR(at);
        return plan_edit{.first_line = at.value(), .last_line = at.value() - 1, .is_insertion = true};
    }

    auto const dash = body.find('-');
    if (dash < 0)
    {
        auto only = parse_line_number(body);
        CC_RETURN_IF_ERROR(only);
        return plan_edit{.first_line = only.value(), .last_line = only.value()};
    }

    auto first = parse_line_number(trim(body.subview({.start = 0, .end = dash})));
    CC_RETURN_IF_ERROR(first);
    auto last = parse_line_number(trim(body.subview(dash + 1)));
    CC_RETURN_IF_ERROR(last);

    if (last.value() < first.value())
        return cc::error(cc::format("span [{}-{}] ends before it starts", first.value(), last.value()));

    return plan_edit{.first_line = first.value(), .last_line = last.value()};
}

/// Spans must read top-down like the file they edit, which is also what makes overlap a local check.
/// An insertion at line n has `last_line == n - 1`, so it may sit directly before a replacement of n.
cc::result<cc::unit> check_follows(plan_edit const& previous, plan_edit const& current)
{
    if (current.first_line > previous.last_line)
        return cc::unit{};

    return cc::error(cc::format("span [{}-{}] overlaps or precedes the previous span [{}-{}]", current.first_line,
                                current.last_line, previous.first_line, previous.last_line));
}
} // namespace

cc::result<prose_plan> parse_prose_plan(cc::string_view text)
{
    prose_plan plan;

    auto pos = isize(0);
    auto line_number = 0;
    while (pos < text.size())
    {
        auto const parsed = next_line(text, pos);
        pos = parsed.next_pos;
        ++line_number;

        auto const line = parsed.line;
        if (is_blank(line))
            continue;

        if (line.starts_with("## "))
        {
            auto const path = trim(line.subview(3));
            if (path.empty())
                return cc::error(cc::format("plan line {}: file header has no path", line_number));

            for (auto const& f : plan.files)
                if (f.path == path)
                    return cc::error(cc::format("plan line {}: '{}' already has a section", line_number, path));

            plan.files.push_back({.path = cc::string(path)});
            continue;
        }

        if (line.starts_with("["))
        {
            if (plan.files.empty())
                return cc::error(cc::format("plan line {}: span before any '## <path>' header", line_number));
            if (line.size() < 2 || !line.ends_with("]"))
                return cc::error(cc::format("plan line {}: span '{}' is missing its ']'", line_number, line));

            auto edit = parse_span_header(line);
            if (edit.has_error())
                return cc::error(cc::format("plan line {}: {}", line_number, edit.error().to_string()));

            auto& file = plan.files.back();
            if (!file.edits.empty())
                CC_RETURN_IF_ERROR(check_follows(file.edits.back(), edit.value()));

            file.edits.push_back(cc::move(edit.value()));
            continue;
        }

        if (line.starts_with("|"))
        {
            if (plan.files.empty() || plan.files.back().edits.empty())
                return cc::error(cc::format("plan line {}: replacement line before any span", line_number));

            // Exactly one space after the pipe is the separator; a bare `|` is an empty line, which is how
            // a blank line inside a comment block is spelled (a truly blank plan line ends the span).
            auto content = line.subview(1);
            if (content.starts_with(" "))
                content = content.subview(1);

            plan.files.back().edits.back().lines.push_back(cc::string(content));
            continue;
        }

        return cc::error(cc::format("plan line {}: expected '## <path>', '[<span>]', '| <text>' or a blank line, got "
                                    "'{}'",
                                    line_number, line));
    }

    return plan;
}
} // namespace scl
