#include <babel-serializer/data/markdown.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/streams/span_stream.hh>

// Line-oriented markdown block parser. One line at a time via read_stream::read_line — never buffers the
// whole file.
//
// Each line goes through the standard three phases: match the already-open containers as far as the
// line's prefixes allow, open whatever new containers the remainder starts, then interpret what is left
// as a leaf (fence / heading / thematic break / paragraph text).
//
// Containers nest, so the parse builds a real tree of `build_node`s with child vectors, then flattens it
// once into the document's preorder arrays. The scratch tree is the parse's only concession — the
// structure handed to the caller is flat, like every other babel reader.
//
// Only the BLOCK grammar is implemented; inline spans are left as raw text. Indentation counts columns
// with tabs at 4-column stops.

namespace babel::impl
{
namespace
{
/// Container nesting cap. The parse recurses once per level when flattening, and markdown that nests
/// deeper than this is pathological rather than authored.
constexpr isize k_max_nesting = 32;

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/// Trim leading/trailing ASCII blanks.
cc::string_view trim(cc::string_view s)
{
    auto a = isize(0);
    auto b = s.size();
    while (a < b && is_space(s[a]))
        ++a;
    while (b > a && is_space(s[b - 1]))
        --b;
    return s.subview({.start = a, .end = b});
}

/// Count the indentation columns starting at `pos` (tab advances to the next multiple of 4) and report
/// the byte index of the first non-blank character in `out_pos`.
i32 scan_indent(cc::string_view s, i32 pos, i32& out_pos)
{
    auto col = i32(0);
    while (pos < s.size())
    {
        if (s[pos] == ' ')
            ++col;
        else if (s[pos] == '\t')
            col += 4 - (col % 4);
        else
            break;
        ++pos;
    }
    out_pos = pos;
    return col;
}

/// Consume at most `want` columns of indentation, returning the byte index reached. Stops early on a tab
/// that would overshoot, so a partially-consumed tab is never split.
i32 advance_indent(cc::string_view s, i32 pos, i32 want)
{
    auto col = i32(0);
    while (pos < s.size() && col < want)
    {
        if (s[pos] == ' ')
            ++col;
        else if (s[pos] == '\t')
        {
            auto const next = col + 4 - (col % 4);
            if (next > want)
                break;
            col = next;
        }
        else
            break;
        ++pos;
    }
    return pos;
}

/// A thematic break: three or more of `-`, `*` or `_`, and nothing else but blanks.
bool is_thematic_break(cc::string_view s, i32 pos)
{
    auto marker = char(0);
    auto count = i32(0);
    for (auto i = isize(pos); i < s.size(); ++i)
    {
        auto const c = s[i];
        if (is_space(c))
            continue;
        if (c != '-' && c != '*' && c != '_')
            return false;
        if (marker == 0)
            marker = c;
        else if (c != marker)
            return false;
        ++count;
    }
    return count >= 3;
}

/// An opening code fence: three or more backticks or tildes. `out_info` is the trimmed info string.
/// A backtick fence may not carry a backtick in its info string — that is inline code, not a fence.
bool scan_fence_open(cc::string_view s, i32 pos, char& out_char, i32& out_length, cc::string_view& out_info)
{
    if (pos >= s.size())
        return false;
    auto const c = s[pos];
    if (c != '`' && c != '~')
        return false;

    auto i = pos;
    while (i < s.size() && s[i] == c)
        ++i;
    auto const length = i - pos;
    if (length < 3)
        return false;

    auto const info = trim(s.subview({.start = i, .end = s.size()}));
    if (c == '`')
        for (auto const ch : info)
            if (ch == '`')
                return false;

    out_char = c;
    out_length = length;
    out_info = info;
    return true;
}

/// A closing fence for an open `marker` x `length` fence: at least as many of the same character, then blanks only.
bool is_fence_close(cc::string_view s, i32 pos, char marker, i32 length)
{
    auto i = pos;
    while (i < s.size() && s[i] == marker)
        ++i;
    if (i - pos < length)
        return false;
    for (auto j = isize(i); j < s.size(); ++j)
        if (!is_space(s[j]))
            return false;
    return true;
}

/// An ATX heading: one to six `#` followed by a blank (or end of line). A trailing run of `#` is stripped.
bool scan_atx_heading(cc::string_view s, i32 pos, u8& out_level, cc::string_view& out_text)
{
    auto i = pos;
    while (i < s.size() && s[i] == '#')
        ++i;
    auto const level = i - pos;
    if (level < 1 || level > 6)
        return false;
    if (i < s.size() && !is_space(s[i]))
        return false; // `#tag` is text, not a heading

    auto text = trim(s.subview({.start = i, .end = s.size()}));
    auto e = text.size();
    while (e > 0 && text[e - 1] == '#')
        --e;
    if (e == 0 || is_space(text[e - 1]))
        text = trim(text.subview({.start = 0, .end = e}));

    out_level = u8(level);
    out_text = text;
    return true;
}

/// The result of looking for a list-item marker at a position.
struct list_marker
{
    bool ok = false;
    char marker = 0;      // `-` / `*` / `+`, or the `.` / `)` delimiter of an ordered item
    bool ordered = false; //
    i32 after = 0;        // byte index where the item's content starts
    i32 content_column = 0;
};

/// A list-item marker at `pos`, whose indentation was `column`. The content column is what a following
/// line must reach to stay inside the item.
list_marker scan_list_marker(cc::string_view s, i32 pos, i32 column)
{
    auto r = list_marker();
    auto i = pos;
    auto width = i32(0);

    if (i < s.size() && (s[i] == '-' || s[i] == '*' || s[i] == '+'))
    {
        r.marker = s[i];
        ++i;
        width = 1;
    }
    else
    {
        auto digits = i32(0);
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 9)
        {
            ++i;
            ++digits;
        }
        if (digits == 0 || i >= s.size() || (s[i] != '.' && s[i] != ')'))
            return r;
        r.marker = s[i];
        r.ordered = true;
        ++i;
        width = digits + 1;
    }

    auto ws_end = i32(0);
    auto const ws = scan_indent(s, i, ws_end);
    if (ws == 0 && i < s.size())
        return r; // the marker must be followed by a blank, or end the line (an empty item)

    // more than four blanks does not indent the content further — only the first one counts
    auto const compact = ws == 0 || ws > 4;
    r.ok = true;
    r.content_column = column + width + (compact ? 1 : ws);
    r.after = compact ? i : ws_end;
    return r;
}

/// One block while parsing. Containers keep their children in a vector; the whole tree is flattened once
/// at the end. `content_indent` and `marker` are build-time bookkeeping and do not reach the document.
struct build_node
{
    markdown::node_kind kind = markdown::node_kind::document;
    u8 level = 0;
    bool ordered = false;
    i32 line = 1;
    cc::string text;
    cc::string info;
    cc::vector<i32> kids;

    i32 content_indent = 0; // list_item: the column its content starts at
    char marker = 0;        // list: which marker opened it, so a different one starts a new list
};

/// Turns the scratch tree into the document's three flat arrays: preorder nodes, contiguous child-index
/// runs, and one text arena.
struct flattener
{
    cc::vector<build_node> const& in;

    cc::vector<markdown::node> out;
    cc::vector<i32> child_indices;
    cc::string text;

    i32 emit(i32 index)
    {
        auto const& b = in[isize(index)];

        auto n = markdown::node{.kind = b.kind, .level = b.level, .ordered = b.ordered, .line = b.line};
        n.text_offset = i32(text.size());
        n.text_length = i32(b.text.size());
        text += b.text;
        n.info_offset = i32(text.size());
        n.info_length = i32(b.info.size());
        text += b.info;

        auto const me = i32(out.size());
        out.push_back(n);

        // children follow in preorder, so their ids are only known after recursing
        auto kid_ids = cc::vector<i32>();
        kid_ids.reserve(b.kids.size());
        for (auto const k : b.kids)
            kid_ids.push_back(emit(k));

        out[isize(me)].first_child = i32(child_indices.size());
        out[isize(me)].child_count = i32(kid_ids.size());
        child_indices.push_back_range(kid_ids);
        return me;
    }
};
} // namespace

struct markdown_parser
{
    cc::vector<build_node> nodes;
    cc::vector<i32> open; // the open container chain, open[0] is always the document root
    i32 open_paragraph = -1;
    i32 line_number = 0;

    // open fenced code block, and the fence that will close it
    i32 open_code = -1;
    char fence_marker = 0;
    i32 fence_length = 0;
    i32 fence_indent = 0;

    markdown_parser()
    {
        nodes.push_back({.kind = markdown::node_kind::document, .line = 1});
        open.push_back(0);
    }

    i32 add_child(i32 parent, build_node n)
    {
        auto const id = i32(nodes.size());
        nodes.push_back(cc::move(n));
        nodes[isize(parent)].kids.push_back(id);
        return id;
    }

    build_node const& node_of(i32 id) const { return nodes[isize(id)]; }
    markdown::node_kind kind_of(i32 id) const { return nodes[isize(id)].kind; }

    /// Whether the remainder starting at `pos` begins a new block — the test that decides whether a line
    /// can lazily continue an open paragraph across a missing container prefix.
    bool starts_new_block(cc::string_view s, i32 pos) const
    {
        auto p = i32(0);
        auto const col = scan_indent(s, pos, p);
        if (p >= s.size())
            return true;
        if (col > 3)
            return false;
        if (s[p] == '>' || is_thematic_break(s, p))
            return true;

        auto marker = char(0);
        auto length = i32(0);
        auto info = cc::string_view();
        if (scan_fence_open(s, p, marker, length, info))
            return true;

        auto level = u8(0);
        auto text = cc::string_view();
        if (scan_atx_heading(s, p, level, text))
            return true;

        return scan_list_marker(s, p, col).ok;
    }

    void append_paragraph_line(cc::string_view s, i32 pos)
    {
        if (open_paragraph < 0)
            open_paragraph = add_child(open.back(), {.kind = markdown::node_kind::paragraph, .line = line_number});

        auto& t = nodes[isize(open_paragraph)].text;
        if (!t.empty())
            t += '\n';
        t += trim(s.subview(pos));
    }

    void feed(cc::string_view raw)
    {
        ++line_number;

        auto line = raw;
        if (!line.empty() && line[line.size() - 1] == '\r')
            line = line.subview({.start = 0, .end = line.size() - 1});

        // 1. match the open containers against this line's prefixes
        auto pos = i32(0);
        auto matched = isize(1); // the document always matches
        for (auto i = isize(1); i < open.size(); ++i)
        {
            auto const& c = node_of(open[i]);

            if (c.kind == markdown::node_kind::block_quote)
            {
                auto p = i32(0);
                auto const col = scan_indent(line, pos, p);
                if (col > 3 || p >= line.size() || line[p] != '>')
                    break;
                pos = p + 1;
                if (pos < line.size() && line[pos] == ' ')
                    ++pos;
                ++matched;
                continue;
            }

            if (c.kind == markdown::node_kind::list_item)
            {
                auto p = i32(0);
                auto const col = scan_indent(line, pos, p);
                if (p >= line.size()) // a blank line stays inside the item
                {
                    ++matched;
                    continue;
                }
                if (col < c.content_indent)
                    break;
                pos = advance_indent(line, pos, c.content_indent);
                ++matched;
                continue;
            }

            // a list itself has no prefix — its open item (checked next) is the real test
            ++matched;
        }

        // 2. inside a fence everything is verbatim until the closing fence
        if (open_code >= 0)
        {
            if (matched >= open.size())
            {
                auto p = i32(0);
                auto const col = scan_indent(line, pos, p);
                if (col <= 3 && is_fence_close(line, p, fence_marker, fence_length))
                {
                    open_code = -1;
                    return;
                }

                auto& t = nodes[isize(open_code)].text;
                t += line.subview(advance_indent(line, pos, fence_indent));
                t += '\n';
                return;
            }
            open_code = -1; // the containers holding the fence ended under it
        }

        // 3. a blank line ends an open paragraph but closes no container
        auto first = i32(0);
        scan_indent(line, pos, first);
        if (first >= line.size())
        {
            open_paragraph = -1;
            return;
        }

        // 4. lazy continuation: a plain text line still belongs to the paragraph it interrupts
        if (matched < open.size() && open_paragraph >= 0 && !starts_new_block(line, pos))
        {
            append_paragraph_line(line, pos);
            return;
        }

        // 5. close whatever the line did not match
        if (open.size() > matched)
        {
            while (open.size() > matched)
                open.pop_back();
            open_paragraph = -1;
        }

        // 6. open the containers the remainder starts
        while (open.size() < k_max_nesting)
        {
            auto p = i32(0);
            auto const col = scan_indent(line, pos, p);
            if (col > 3 || p >= line.size())
                break;

            if (line[p] == '>')
            {
                pos = p + 1;
                if (pos < line.size() && line[pos] == ' ')
                    ++pos;
                open.push_back(add_child(open.back(), {.kind = markdown::node_kind::block_quote, .line = line_number}));
                open_paragraph = -1;
                continue;
            }

            if (is_thematic_break(line, p)) // `---` is a break, never a bullet
                break;

            auto const m = scan_list_marker(line, p, col);
            if (!m.ok)
                break;

            // reuse the enclosing list when the marker type matches, otherwise start a fresh one
            if (kind_of(open.back()) != markdown::node_kind::list || node_of(open.back()).marker != m.marker)
            {
                if (kind_of(open.back()) == markdown::node_kind::list)
                    open.pop_back();
                open.push_back(add_child(
                    open.back(),
                    {.kind = markdown::node_kind::list, .ordered = m.ordered, .line = line_number, .marker = m.marker}));
            }
            open.push_back(add_child(
                open.back(),
                {.kind = markdown::node_kind::list_item, .line = line_number, .content_indent = m.content_column}));
            pos = m.after;
            open_paragraph = -1;
        }

        // a list with no open item holds no content of its own
        while (open.size() > 1 && kind_of(open.back()) == markdown::node_kind::list)
            open.pop_back();

        // 7. the leaf
        auto p = i32(0);
        auto const col = scan_indent(line, pos, p);

        if (col <= 3 && is_thematic_break(line, p))
        {
            add_child(open.back(), {.kind = markdown::node_kind::thematic_break, .line = line_number});
            open_paragraph = -1;
            return;
        }

        auto marker = char(0);
        auto length = i32(0);
        auto info = cc::string_view();
        if (col <= 3 && scan_fence_open(line, p, marker, length, info))
        {
            open_code = add_child(
                open.back(), {.kind = markdown::node_kind::code_block, .line = line_number, .info = cc::string(info)});
            fence_marker = marker;
            fence_length = length;
            fence_indent = col;
            open_paragraph = -1;
            return;
        }

        auto level = u8(0);
        auto text = cc::string_view();
        if (col <= 3 && scan_atx_heading(line, p, level, text))
        {
            add_child(
                open.back(),
                {.kind = markdown::node_kind::heading, .level = level, .line = line_number, .text = cc::string(text)});
            open_paragraph = -1;
            return;
        }

        append_paragraph_line(line, pos);
    }

    [[nodiscard]] cc::result<markdown::document> parse(cc::read_stream& in)
    {
        auto line = cc::string();
        while (true)
        {
            auto more = in.read_line(line);
            CC_RETURN_IF_ERROR(more);
            if (!more.value())
                break;
            feed(line);
        }

        auto f = flattener{.in = nodes};
        f.emit(0);
        return markdown::document(cc::move(f.out), cc::move(f.child_indices), cc::move(f.text));
    }
};
} // namespace babel::impl

namespace babel::markdown
{
cc::result<document> read(cc::read_stream& in)
{
    auto parser = babel::impl::markdown_parser();
    return parser.parse(in);
}

cc::result<document> read(cc::span<cc::byte const> bytes)
{
    auto adapter = cc::span_read_stream_adapter(bytes);
    cc::read_stream stream = adapter;
    return read(stream);
}

cc::result<document> read(cc::string_view text)
{
    return read(cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(text.data()), text.size()));
}
} // namespace babel::markdown
