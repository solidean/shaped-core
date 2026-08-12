#include "config_parser.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

namespace scl
{
namespace
{
/// A line reduced to what the grammar cares about: how deep it sits and what is on it.
/// Blank and comment-only lines never make it into the vector, so the parser never has to skip.
struct config_line
{
    u32 number = 0; // 1-based
    isize indent = 0;
    cc::string_view text; // from the first non-space to the last, comment already removed
};

cc::string_view trim(cc::string_view s)
{
    auto begin = isize(0);
    while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\t'))
        ++begin;
    auto end = s.size();
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t'))
        --end;
    return s.subview({.start = begin, .end = end});
}

/// Where a line's content ends: at a `#` that opens a comment, or at the line's end.
/// A `#` inside quotes is content, and one glued to a word (`c#`) is too — only a `#` at the start or
/// after whitespace opens a comment, which is what keeps a `#include` spelled in a reason intact.
isize content_end(cc::string_view line)
{
    auto quote = char(0);
    for (auto i = isize(0); i < line.size(); ++i)
    {
        auto const c = line[i];
        if (quote != 0)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"')
            quote = c;
        else if (c == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t'))
            return i;
    }
    return line.size();
}

/// The scalar a value line spells: quotes stripped if it is quoted end to end, trimmed otherwise.
/// Nothing inside quotes is interpreted — there are no escapes, so a path or a reason is verbatim.
cc::string_view unquote(cc::string_view s)
{
    if (s.size() >= 2 && (s[0] == '\'' || s[0] == '"') && s[s.size() - 1] == s[0])
        return s.subview({.start = 1, .end = s.size() - 1});
    return s;
}

/// The offset of the `:` that ends a key, or -1 when the line opens no entry.
/// A `:` inside quotes is not it, and neither is one glued to the next word — `a:b` is a scalar, `a: b` is an entry.
/// That is what lets a value hold a `::` without quoting.
isize key_separator(cc::string_view s)
{
    auto quote = char(0);
    for (auto i = isize(0); i < s.size(); ++i)
    {
        auto const c = s[i];
        if (quote != 0)
        {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"')
            quote = c;
        else if (c == ':' && (i + 1 == s.size() || s[i + 1] == ' '))
            return i;
    }
    return -1;
}

struct config_parser
{
    cc::vector<config_line> lines;
    isize pos = 0;
    config_document doc;

    bool at_end() const { return pos >= lines.size(); }
    config_line const& peek() const { return lines[pos]; }

    cc::string error_at(config_line const& l, isize column, cc::string_view message) const
    {
        return cc::format("line {}, column {}: {}", l.number, column, message);
    }

    isize add(config_value_kind kind, u32 line)
    {
        doc.nodes.push_back({.kind = kind, .line = line});
        return doc.nodes.size() - 1;
    }

    /// The value written on the same line as its key, after the `:`.
    /// `[a, b]` is a flow list; anything else is one scalar.
    cc::result<isize> parse_inline_value(config_line const& l, cc::string_view text)
    {
        if (!text.starts_with('['))
            return add_scalar(l.number, unquote(text));

        if (!text.ends_with(']'))
            return cc::error(error_at(l, l.indent + 1, "unterminated flow list — expected a closing ']'"));

        auto const id = add(config_value_kind::list, l.number);
        auto const inner = trim(text.subview({.start = 1, .end = text.size() - 1}));
        if (inner.empty())
            return id;

        auto item_begin = isize(0);
        for (auto i = isize(0); i <= inner.size(); ++i)
        {
            if (i < inner.size() && inner[i] != ',')
                continue;
            auto const item = trim(inner.subview({.start = item_begin, .end = i}));
            if (item.empty())
                return cc::error(error_at(l, l.indent + 1, "empty item in a flow list"));
            // The child id is taken first: adding a node may grow the arena, and `doc.nodes[id]` is
            // sequenced before the argument that would invalidate it.
            auto const child = add_scalar(l.number, unquote(item));
            doc.nodes[id].children.push_back(child);
            item_begin = i + 1;
        }
        return id;
    }

    isize add_scalar(u32 line, cc::string_view text)
    {
        auto const id = add(config_value_kind::scalar, line);
        doc.nodes[id].scalar = cc::string(text);
        return id;
    }

    /// The block under a key that ended its line: whichever of a list or a mapping starts below it.
    cc::result<isize> parse_block(config_line const& key_line)
    {
        if (at_end() || peek().indent <= key_line.indent)
            return cc::error(error_at(key_line, key_line.indent + 1, "expected a value, or an indented block below it"));

        auto const indent = peek().indent;
        if (peek().text.starts_with("- ") || peek().text == "-")
            return parse_list(indent);
        return parse_mapping(indent);
    }

    /// A run of `- ` items, all at `indent`.
    cc::result<isize> parse_list(isize indent)
    {
        auto const id = add(config_value_kind::list, peek().number);
        while (!at_end() && peek().indent == indent && (peek().text.starts_with("- ") || peek().text == "-"))
        {
            auto& l = lines[pos];
            auto const item_text = trim(l.text.subview(1));
            if (item_text.empty())
                return cc::error(error_at(l, indent + 1, "a list item must carry a value"));

            // An item that opens an entry IS a mapping, and its siblings are the lines aligned under the key rather than under the dash.
            // Rewriting the line in place is what lets parse_mapping own the whole item, dash and all, with no second code path for the first entry.
            auto const dash_width = l.text.size() - item_text.size();
            l.indent = indent + dash_width;
            l.text = item_text;

            if (key_separator(item_text) >= 0)
            {
                auto item = parse_mapping(l.indent);
                CC_RETURN_IF_ERROR(item);
                doc.nodes[id].children.push_back(item.value());
            }
            else
            {
                auto const child = add_scalar(l.number, unquote(item_text));
                doc.nodes[id].children.push_back(child);
                ++pos;
            }
        }
        return id;
    }

    /// A run of `key: …` entries, all at `indent`.
    cc::result<isize> parse_mapping(isize indent)
    {
        auto const id = add(config_value_kind::mapping, peek().number);
        while (!at_end() && peek().indent == indent)
        {
            auto const& l = peek();
            auto const sep = key_separator(l.text);
            if (sep < 0)
                return cc::error(error_at(l, indent + 1, cc::format("expected 'key: value', found '{}'", l.text)));

            auto const key = trim(l.text.subview({.start = 0, .end = sep}));
            if (key.empty())
                return cc::error(error_at(l, indent + 1, "an entry must have a key before its ':'"));

            auto const rest = trim(l.text.subview(sep + 1));
            ++pos;

            auto value = rest.empty() ? parse_block(l) : parse_inline_value(l, rest);
            CC_RETURN_IF_ERROR(value);

            doc.nodes[id].keys.push_back(cc::string(unquote(key)));
            doc.nodes[id].children.push_back(value.value());
        }

        // A deeper line here belongs to nothing: its key ended a line above and was consumed, or it is simply over-indented.
        // Either way it is a typo, and silently dropping it would lose a rule.
        if (!at_end() && peek().indent > indent)
            return cc::error(
                error_at(peek(), peek().indent + 1, "unexpected indentation — this line has no key above it"));

        return id;
    }
};

/// Split into content lines, rejecting a tab before the parser ever sees one.
cc::result<cc::vector<config_line>> split_lines(cc::string_view text)
{
    cc::vector<config_line> out;
    auto number = u32(0);
    auto pos = isize(0);

    while (pos <= text.size())
    {
        auto end = pos;
        while (end < text.size() && text[end] != '\n')
            ++end;
        auto content = text.subview({.start = pos, .end = end});
        if (content.ends_with('\r'))
            content = content.subview({.start = 0, .end = content.size() - 1});
        ++number;

        auto indent = isize(0);
        while (indent < content.size() && content[indent] == ' ')
            ++indent;
        if (indent < content.size() && content[indent] == '\t')
            return cc::error(cc::format("line {}, column {}: indentation must be spaces, not a tab", number, indent + 1));

        auto const body = trim(content.subview({.start = 0, .end = content_end(content)}));
        if (!body.empty())
            out.push_back({.number = number, .indent = indent, .text = body});

        if (end >= text.size())
            break;
        pos = end + 1;
    }
    return out;
}
} // namespace

isize config_document::find(isize id, cc::string_view key) const
{
    auto const& n = nodes[id];
    for (auto i = isize(0); i < n.keys.size(); ++i)
        if (n.keys[i] == key)
            return n.children[i];
    return -1;
}

cc::result<config_document> parse_config(cc::string_view text)
{
    auto lines = split_lines(text);
    CC_RETURN_IF_ERROR(lines);

    config_parser p;
    p.lines = cc::move(lines.value());
    if (p.lines.empty())
        return cc::move(p.doc); // an empty file is a valid config that says nothing

    if (p.peek().indent != 0)
        return cc::error(cc::format("line {}, column {}: the first entry must start at column 1", p.peek().number,
                                    p.peek().indent + 1));

    auto root = p.parse_mapping(0);
    CC_RETURN_IF_ERROR(root);
    p.doc.root = root.value();

    CC_ASSERT(p.at_end(), "parse_mapping must consume every line or fail");
    return cc::move(p.doc);
}
} // namespace scl
