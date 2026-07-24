#include <babel-serializer/data/json.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>

#include <charconv> // std::from_chars

// Streaming recursive-descent JSON parser.
//
// It reads bytes straight out of the stream's buffered window (ready_bytes / consume / flush) through a tiny
// peek/advance cursor — it never materializes the whole input. Values are appended to `nodes` in preorder; a
// container's direct children are not contiguous in `nodes` (a child subtree sits between them), so each child's
// node index is gathered on the C++ call stack and committed as one contiguous block into `child_indices`, which
// is what gives O(1) child access. String and key bytes are unescaped once into the `text` arena.
//
// Error handling mirrors a sticky cursor: the first malformed token records a message and every later parse
// short-circuits, so the recursion unwinds cheaply.

namespace babel::impl
{
struct json_parser
{
    cc::read_stream* in = nullptr;

    // current buffered window and our position within it; base_offset counts bytes consumed before the window
    cc::span<cc::byte const> window;
    isize wpos = 0;
    i64 base_offset = 0;

    bool failed = false;
    cc::string message;

    cc::vector<json::node> nodes;
    cc::vector<i32> child_indices;
    cc::string text;

    [[nodiscard]] i64 offset() const { return base_offset + wpos; }

    void fail(cc::string_view what)
    {
        if (!failed) // keep the first error (an I/O failure recorded by ensure() must not be overwritten)
        {
            failed = true;
            message = cc::format("JSON parse error at offset {}: {}", offset(), what);
        }
    }

    // cursor
    // ---------------------------------------------------------------------------------------------

    /// Make at least one byte available at window[wpos], refilling across flushes. False at end of data / on I/O error.
    [[nodiscard]] bool ensure()
    {
        while (wpos >= window.size())
        {
            in->consume(window.size()); // done with this window
            base_offset += window.size();
            wpos = 0;

            auto const pos = in->flush();
            if (pos.has_error())
            {
                if (!failed)
                {
                    failed = true;
                    message = cc::format("JSON read error at offset {}: stream failure", offset());
                }
                return false;
            }
            window = in->ready_bytes();
            if (window.empty())
                return false; // genuine end of data
        }
        return true;
    }

    [[nodiscard]] int peek() { return ensure() ? int(static_cast<unsigned char>(window[wpos])) : -1; }

    void bump() { ++wpos; }

    [[nodiscard]] int next()
    {
        auto const c = peek();
        if (c >= 0)
            bump();
        return c;
    }

    void skip_ws()
    {
        while (true)
        {
            auto const c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                bump();
            else
                return;
        }
    }

    // building
    // ---------------------------------------------------------------------------------------------

    [[nodiscard]] i32 add_node(json::node n)
    {
        auto const index = i32(nodes.size());
        nodes.push_back(n);
        return index;
    }

    // commit the gathered child indices as one contiguous block and record the range on the container node
    void commit_children(i32 container, cc::span<i32 const> local)
    {
        nodes[isize(container)].first_child = i32(child_indices.size());
        nodes[isize(container)].child_count = i32(local.size());
        for (auto const ci : local)
            child_indices.push_back(ci);
    }

    // scalars
    // ---------------------------------------------------------------------------------------------

    static void encode_utf8(u32 cp, cc::string& out)
    {
        if (cp <= 0x7f)
            out += char(cp);
        else if (cp <= 0x7ff)
        {
            out += char(0xc0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3f));
        }
        else if (cp <= 0xffff)
        {
            out += char(0xe0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
        else
        {
            out += char(0xf0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3f));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
    }

    [[nodiscard]] bool parse_hex4(u32& out)
    {
        auto v = u32(0);
        for (auto i = 0; i < 4; ++i)
        {
            auto const c = next();
            if (c < 0)
            {
                fail("truncated \\u escape");
                return false;
            }
            auto const ch = char(c);
            v <<= 4;
            if ('0' <= ch && ch <= '9')
                v |= u32(ch - '0');
            else if ('a' <= ch && ch <= 'f')
                v |= u32(ch - 'a' + 10);
            else if ('A' <= ch && ch <= 'F')
                v |= u32(ch - 'A' + 10);
            else
            {
                fail("invalid hex digit in \\u escape");
                return false;
            }
        }
        out = v;
        return true;
    }

    // parse a string (cursor at the opening quote) directly into the arena; returns its slice.
    [[nodiscard]] bool parse_string(i32& out_offset, i32& out_length)
    {
        bump(); // opening quote
        auto const start = i32(text.size());
        while (true)
        {
            auto const c = next();
            if (c < 0)
            {
                fail("unterminated string");
                return false;
            }
            if (c == '"')
                break;
            if (c != '\\')
            {
                text += char(c);
                continue;
            }

            auto const e = next();
            switch (e)
            {
            case '"':
                text += '"';
                break;
            case '\\':
                text += '\\';
                break;
            case '/':
                text += '/';
                break;
            case 'b':
                text += '\b';
                break;
            case 'f':
                text += '\f';
                break;
            case 'n':
                text += '\n';
                break;
            case 'r':
                text += '\r';
                break;
            case 't':
                text += '\t';
                break;
            case 'u':
            {
                auto hi = u32(0);
                if (!parse_hex4(hi))
                    return false;
                auto cp = hi;
                if (0xd800 <= hi && hi <= 0xdbff) // high surrogate must be followed by "\uXXXX" low surrogate
                {
                    if (next() != '\\' || next() != 'u')
                    {
                        fail("unpaired high surrogate");
                        return false;
                    }
                    auto lo = u32(0);
                    if (!parse_hex4(lo))
                        return false;
                    if (!(0xdc00 <= lo && lo <= 0xdfff))
                    {
                        fail("invalid low surrogate");
                        return false;
                    }
                    cp = 0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00);
                }
                encode_utf8(cp, text);
                break;
            }
            default:
                fail("invalid escape sequence");
                return false;
            }
        }
        out_offset = start;
        out_length = i32(text.size()) - start;
        return true;
    }

    [[nodiscard]] i32 parse_number()
    {
        auto scratch = cc::string();
        auto c = peek();
        if (c == '-' || c == '+')
        {
            scratch += char(c);
            bump();
            c = peek();
        }
        while (c >= 0 && (cc::is_digit(char(c)) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'))
        {
            scratch += char(c);
            bump();
            c = peek();
        }

        auto value = double(0);
        auto const* const first = scratch.data();
        auto const* const last = scratch.data() + scratch.size();
        auto const [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc() || ptr != last)
        {
            fail("invalid number");
            return -1;
        }
        return add_node({.kind = json::node_kind::number, .number = value});
    }

    [[nodiscard]] i32 parse_keyword(cc::string_view word, json::node value)
    {
        for (auto const w : word)
        {
            if (next() != int(static_cast<unsigned char>(w)))
            {
                fail("invalid literal");
                return -1;
            }
        }
        return add_node(value);
    }

    // composites
    // ---------------------------------------------------------------------------------------------

    [[nodiscard]] i32 parse_array()
    {
        bump(); // '['
        auto const me = add_node({.kind = json::node_kind::array});
        auto local = cc::vector<i32>();

        skip_ws();
        if (peek() == ']')
        {
            bump();
            return me;
        }
        while (true)
        {
            auto const child = parse_value();
            if (failed)
                return -1;
            local.push_back(child);

            skip_ws();
            auto const c = next();
            if (c == ',')
                continue;
            if (c == ']')
                break;
            fail("expected ',' or ']' in array");
            return -1;
        }
        commit_children(me, local);
        return me;
    }

    [[nodiscard]] i32 parse_object()
    {
        bump(); // '{'
        auto const me = add_node({.kind = json::node_kind::object});
        auto local = cc::vector<i32>();

        skip_ws();
        if (peek() == '}')
        {
            bump();
            return me;
        }
        while (true)
        {
            skip_ws();
            if (peek() != '"')
            {
                fail("expected string key in object");
                return -1;
            }
            auto key_offset = i32(0);
            auto key_length = i32(0);
            if (!parse_string(key_offset, key_length))
                return -1;

            skip_ws();
            if (next() != ':')
            {
                fail("expected ':' after object key");
                return -1;
            }

            auto const child = parse_value();
            if (failed)
                return -1;
            nodes[isize(child)].key_offset = key_offset;
            nodes[isize(child)].key_length = key_length;
            local.push_back(child);

            skip_ws();
            auto const c = next();
            if (c == ',')
                continue;
            if (c == '}')
                break;
            fail("expected ',' or '}' in object");
            return -1;
        }
        commit_children(me, local);
        return me;
    }

    [[nodiscard]] i32 parse_value()
    {
        skip_ws();
        auto const c = peek();
        if (c < 0)
        {
            fail("unexpected end of input");
            return -1;
        }
        switch (c)
        {
        case '{':
            return parse_object();
        case '[':
            return parse_array();
        case '"':
        {
            auto str_offset = i32(0);
            auto str_length = i32(0);
            if (!parse_string(str_offset, str_length))
                return -1;
            return add_node({.kind = json::node_kind::string, .str_offset = str_offset, .str_length = str_length});
        }
        case 't':
            return parse_keyword("true", {.kind = json::node_kind::boolean, .boolean = true});
        case 'f':
            return parse_keyword("false", {.kind = json::node_kind::boolean, .boolean = false});
        case 'n':
            return parse_keyword("null", {.kind = json::node_kind::null});
        default:
            return parse_number();
        }
    }

    [[nodiscard]] cc::result<json::document> parse()
    {
        auto const root = parse_value();
        if (failed)
            return cc::error(cc::move(message));
        (void)root; // on success root == 0; kept for clarity

        skip_ws();
        if (peek() >= 0)
        {
            fail("trailing characters after top-level value");
            return cc::error(cc::move(message));
        }
        if (failed) // an I/O error during the trailing skip
            return cc::error(cc::move(message));

        return json::document(cc::move(nodes), cc::move(child_indices), cc::move(text));
    }
};
} // namespace babel::impl

namespace babel::json
{
cc::result<document> read(cc::read_stream& in)
{
    auto parser = babel::impl::json_parser();
    parser.in = &in;
    return parser.parse();
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
} // namespace babel::json
