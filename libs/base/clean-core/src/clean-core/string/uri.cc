#include "uri.hh"

#include <clean-core/string/char_predicates.hh>

namespace cc
{
namespace
{
constexpr bool is_alpha(char c)
{
    return cc::is_lower(c) || cc::is_upper(c);
}

/// scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), RFC 3986 section 3.1.
constexpr bool is_scheme_char(char c)
{
    return cc::is_alphanumeric(c) || c == '+' || c == '-' || c == '.';
}

/// sub-delims, RFC 3986 section 2.2.
constexpr bool is_sub_delim(char c)
{
    switch (c)
    {
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
        return true;
    default:
        return false;
    }
}

/// unreserved, RFC 3986 section 2.3 -- never escaped by anything, and safe to unescape anywhere.
constexpr bool is_unreserved(char c)
{
    return cc::is_alphanumeric(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

constexpr i32 hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

constexpr char hex_digit_upper(i32 v)
{
    return v < 10 ? char('0' + v) : char('A' + (v - 10));
}

void append_escaped(cc::string& out, char c)
{
    auto const b = u8(c);
    out.push_back('%');
    out.push_back(hex_digit_upper(i32(b >> 4)));
    out.push_back(hex_digit_upper(i32(b & 0xF)));
}

/// Whether `c` may appear literally in `component`, beyond the unreserved set every component allows.
constexpr bool is_allowed_extra(char c, uri_component component)
{
    switch (component)
    {
    case uri_component::path:
        return is_sub_delim(c) || c == ':' || c == '@' || c == '/';
    case uri_component::path_segment:
        return is_sub_delim(c) || c == ':' || c == '@';
    case uri_component::query:
    case uri_component::fragment:
        return is_sub_delim(c) || c == ':' || c == '@' || c == '/' || c == '?';
    case uri_component::userinfo:
        return is_sub_delim(c) || c == ':';
    case uri_component::host:
        return is_sub_delim(c);
    case uri_component::form:
        return false;
    }
    return false;
}

/// A URI carries no whitespace and no control bytes; both are how one URI silently becomes two.
/// Bytes >= 0x80 are tolerated rather than rejected: they are not legal RFC 3986, and real documents carry them.
constexpr bool is_forbidden_raw(char c)
{
    auto const b = u8(c);
    return b <= 0x20 || b == 0x7F;
}

/// Every `%` in `text` must begin a complete two-digit escape.
bool escapes_are_well_formed(string_view text)
{
    for (isize i = 0; i < text.size(); ++i)
    {
        if (text[i] != '%')
            continue;
        if (i + 2 >= text.size())
            return false;
        if (hex_value(text[i + 1]) < 0 || hex_value(text[i + 2]) < 0)
            return false;
        i += 2;
    }
    return true;
}

/// Index of the first character of `set` at or after `from`, or `text.size()` when there is none.
isize find_first_of(string_view text, string_view set, isize from)
{
    for (auto i = from; i < text.size(); ++i)
        for (auto const c : set)
            if (text[i] == c)
                return i;
    return text.size();
}
} // namespace

// ---- parsing -------------------------------------------------------------------------------------------

uri_view uri_view::from_parts(string_view text, impl::uri_parts const& parts)
{
    uri_view v;
    v._text = text;
    v._parts = parts;
    return v;
}

optional<uri_view> uri_view::parse(string_view text)
{
    for (auto const c : text)
        if (is_forbidden_raw(c))
            return {};

    if (!escapes_are_well_formed(text))
        return {};

    auto parts = impl::uri_parts();
    isize pos = 0;

    // scheme, when the run of scheme characters starts with a letter and ends at a ':'
    if (!text.empty() && is_alpha(text[0]))
    {
        isize i = 0;
        while (i < text.size() && is_scheme_char(text[i]))
            ++i;
        if (i < text.size() && text[i] == ':')
        {
            parts.scheme_end = i32(i);
            pos = i + 1;
        }
    }

    // authority, when the remainder opens with "//"
    if (pos + 1 < text.size() && text[pos] == '/' && text[pos + 1] == '/')
    {
        auto const authority_begin = pos + 2;
        auto const authority_end = find_first_of(text, "/?#", authority_begin);

        parts.authority_begin = i32(authority_begin);
        parts.host_begin = i32(authority_begin);

        // The LAST '@' ends the userinfo: an '@' may appear within it, escaped or as a sub-delimiter.
        for (auto i = authority_end - 1; i >= authority_begin; --i)
        {
            if (text[i] == '@')
            {
                parts.userinfo_end = i32(i);
                parts.host_begin = i32(i + 1);
                break;
            }
        }

        // A port is the ':' after the host.
        // An IPv6 literal is full of colons, so skip past its ']' first.
        auto colon_search_from = isize(parts.host_begin);
        if (colon_search_from < authority_end && text[colon_search_from] == '[')
        {
            auto const close = find_first_of(text, "]", colon_search_from);
            if (close >= authority_end)
                return {}; // an unterminated IPv6 literal
            colon_search_from = close + 1;
        }
        for (auto i = colon_search_from; i < authority_end; ++i)
        {
            if (text[i] == ':')
            {
                parts.port_begin = i32(i + 1);
                break;
            }
        }
        if (parts.port_begin >= 0)
            for (auto i = isize(parts.port_begin); i < authority_end; ++i)
                if (!cc::is_digit(text[i]))
                    return {};

        parts.path_begin = i32(authority_end);
        pos = authority_end;
    }
    else
    {
        parts.path_begin = i32(pos);
    }

    // The fragment is found first, so a '?' inside it is not mistaken for the query delimiter.
    auto const hash = find_first_of(text, "#", pos);
    if (hash < text.size())
        parts.fragment_begin = i32(hash + 1);

    auto const question = find_first_of(text.subview({.offset = 0, .size = hash}), "?", pos);
    if (question < hash)
        parts.query_begin = i32(question + 1);

    return uri_view::from_parts(text, parts);
}

string_view uri_view::authority() const
{
    if (_parts.authority_begin < 0)
        return {};
    return _text.subview({.start = _parts.authority_begin, .end = _parts.path_begin});
}

string_view uri_view::userinfo() const
{
    if (_parts.userinfo_end < 0)
        return {};
    return _text.subview({.start = _parts.authority_begin, .end = _parts.userinfo_end});
}

string_view uri_view::host() const
{
    if (_parts.authority_begin < 0)
        return {};
    auto const host_end = _parts.port_begin >= 0 ? _parts.port_begin - 1 : _parts.path_begin;
    return _text.subview({.start = _parts.host_begin, .end = host_end});
}

string_view uri_view::port_text() const
{
    if (_parts.port_begin < 0)
        return {};
    return _text.subview({.start = _parts.port_begin, .end = _parts.path_begin});
}

optional<i32> uri_view::port() const
{
    auto const t = port_text();
    if (t.empty())
        return {};

    i64 value = 0;
    for (auto const c : t)
    {
        value = value * 10 + (c - '0');
        if (value > 0xFFFF)
            return {};
    }
    return i32(value);
}

string_view uri_view::path() const
{
    auto path_end = _text.size();
    if (_parts.query_begin >= 0)
        path_end = _parts.query_begin - 1;
    else if (_parts.fragment_begin >= 0)
        path_end = _parts.fragment_begin - 1;
    return _text.subview({.start = _parts.path_begin, .end = path_end});
}

string_view uri_view::query() const
{
    if (_parts.query_begin < 0)
        return {};
    auto const query_end = _parts.fragment_begin >= 0 ? _parts.fragment_begin - 1 : _text.size();
    return _text.subview({.start = _parts.query_begin, .end = query_end});
}

string_view uri_view::fragment() const
{
    if (_parts.fragment_begin < 0)
        return {};
    return _text.subview(_parts.fragment_begin);
}

// ---- the owning counterpart ----------------------------------------------------------------------------

uri uri::from_parsed(string text, impl::uri_parts const& parts)
{
    uri u;
    u._text = cc::move(text);
    u._parts = parts;
    return u;
}

optional<uri> uri::parse(string_view text)
{
    auto const v = uri_view::parse(text);
    if (!v.has_value())
        return {};
    return uri::from_parsed(string(text), v.value().parts());
}

// ---- percent-encoding ----------------------------------------------------------------------------------

string percent_encode(string_view s, uri_component component)
{
    auto out = string();
    out.reserve_back(s.size());
    for (auto const c : s)
    {
        if (is_unreserved(c) || is_allowed_extra(c, component))
            out.push_back(c);
        else if (component == uri_component::form && c == ' ')
            out.push_back('+');
        else
            append_escaped(out, c);
    }
    return out;
}

namespace
{
optional<string> percent_decode_impl(string_view s, bool plus_is_space)
{
    auto out = string();
    out.reserve_back(s.size());
    for (isize i = 0; i < s.size(); ++i)
    {
        auto const c = s[i];
        if (c == '%')
        {
            if (i + 2 >= s.size())
                return {};
            auto const hi = hex_value(s[i + 1]);
            auto const lo = hex_value(s[i + 2]);
            if (hi < 0 || lo < 0)
                return {};
            out.push_back(char(u8(hi * 16 + lo)));
            i += 2;
        }
        else if (plus_is_space && c == '+')
        {
            out.push_back(' ');
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}
} // namespace

optional<string> percent_decode(string_view s)
{
    return percent_decode_impl(s, false);
}

optional<string> percent_decode_form(string_view s)
{
    return percent_decode_impl(s, true);
}

// ---- query parameters ----------------------------------------------------------------------------------

namespace
{
uri_query_parameter split_parameter(string_view item)
{
    auto const eq = item.find('=');
    if (eq < 0)
        return {.name = item, .value = {}, .has_value = false};
    return {.name = item.subview({.offset = 0, .size = eq}), .value = item.subview(eq + 1), .has_value = true};
}
} // namespace

vector<uri_query_parameter> parse_query_parameters(string_view query)
{
    auto out = vector<uri_query_parameter>();
    isize begin = 0;
    while (begin <= query.size())
    {
        auto end = query.find('&', begin);
        if (end < 0)
            end = query.size();
        if (end > begin)
            out.push_back(split_parameter(query.subview({.start = begin, .end = end})));
        begin = end + 1;
    }
    return out;
}

optional<string_view> find_query_parameter(string_view query, string_view name)
{
    isize begin = 0;
    while (begin <= query.size())
    {
        auto end = query.find('&', begin);
        if (end < 0)
            end = query.size();
        if (end > begin)
        {
            auto const p = split_parameter(query.subview({.start = begin, .end = end}));
            if (p.name == name)
                return p.value;
        }
        begin = end + 1;
    }
    return {};
}

// ---- path algebra --------------------------------------------------------------------------------------

string remove_dot_segments(string_view path)
{
    // RFC 3986 section 5.2.4, written as the specification writes it: a five-way match on the head of the input.
    auto out = string();
    auto in = path;

    auto const remove_last_segment = [&out]
    {
        auto const slash = string_view(out).rfind('/');
        if (slash < 0)
            out.clear();
        else
            out.resize_down_to(slash);
    };

    while (!in.empty())
    {
        if (in.starts_with("../"))
            in.remove_prefix(3);
        else if (in.starts_with("./"))
            in.remove_prefix(2);
        else if (in.starts_with("/./"))
            in.remove_prefix(2); // leaves the '/' that replaces the whole "/./"
        else if (in == "/.")
            in = "/";
        else if (in.starts_with("/../"))
        {
            in.remove_prefix(3);
            remove_last_segment();
        }
        else if (in == "/..")
        {
            in = "/";
            remove_last_segment();
        }
        else if (in == "." || in == "..")
        {
            in = {};
        }
        else
        {
            // Move the first segment, including a leading '/', to the output.
            auto end = in.starts_with('/') ? in.find('/', 1) : in.find('/');
            if (end < 0)
                end = in.size();
            out += in.subview({.offset = 0, .size = end});
            in = in.subview(end);
        }
    }
    return out;
}

// ---- reference resolution ------------------------------------------------------------------------------

namespace
{
/// RFC 3986 section 5.2.3: merge a relative path onto the base's.
string merge_paths(uri_view base, string_view reference_path)
{
    if (base.has_authority() && base.path().empty())
        return string("/") + reference_path;

    auto const base_path = base.path();
    auto const slash = base_path.rfind('/');
    if (slash < 0)
        return string(reference_path);
    return string(base_path.subview({.offset = 0, .size = slash + 1})) + reference_path;
}

void append_authority(string& out, uri_view v)
{
    out += "//";
    if (v.has_userinfo())
    {
        out += v.userinfo();
        out.push_back('@');
    }
    out += v.host();
    if (v.has_port())
    {
        out.push_back(':');
        out += v.port_text();
    }
}
} // namespace

optional<uri> uri::resolve(string_view reference) const
{
    auto const base = view();
    if (!base.is_absolute())
        return {};

    auto const parsed = uri_view::parse(reference);
    if (!parsed.has_value())
        return {};
    auto const ref = parsed.value();

    // RFC 3986 section 5.2.2, strict: a reference that carries a scheme is already the answer.
    if (ref.is_absolute())
        return uri::parse(ref.text());

    auto out = string();
    out += base.scheme();
    out.push_back(':');

    if (ref.has_authority())
    {
        append_authority(out, ref);
        out += remove_dot_segments(ref.path());
    }
    else
    {
        if (base.has_authority())
            append_authority(out, base);

        if (ref.path().empty())
            out += base.path();
        else if (ref.path().starts_with('/'))
            out += remove_dot_segments(ref.path());
        else
            out += remove_dot_segments(merge_paths(base, ref.path()));
    }

    // An empty reference path with no query of its own inherits the base's query, and only then.
    if (ref.has_query())
    {
        out.push_back('?');
        out += ref.query();
    }
    else if (ref.path().empty() && !ref.has_authority() && base.has_query())
    {
        out.push_back('?');
        out += base.query();
    }

    if (ref.has_fragment())
    {
        out.push_back('#');
        out += ref.fragment();
    }

    return uri::parse(out);
}

// ---- normalization -------------------------------------------------------------------------------------

namespace
{
/// Uppercase every escape's hex digits, and decode the ones that spell an unreserved character.
string normalize_escapes(string_view s)
{
    auto out = string();
    out.reserve_back(s.size());
    for (isize i = 0; i < s.size(); ++i)
    {
        if (s[i] != '%' || i + 2 >= s.size())
        {
            out.push_back(s[i]);
            continue;
        }

        auto const hi = hex_value(s[i + 1]);
        auto const lo = hex_value(s[i + 2]);
        if (hi < 0 || lo < 0)
        {
            out.push_back(s[i]);
            continue;
        }

        auto const decoded = char(u8(hi * 16 + lo));
        if (is_unreserved(decoded))
        {
            out.push_back(decoded);
        }
        else
        {
            out.push_back('%');
            out.push_back(hex_digit_upper(hi));
            out.push_back(hex_digit_upper(lo));
        }
        i += 2;
    }
    return out;
}

string to_lower_ascii(string_view s)
{
    auto out = string();
    out.reserve_back(s.size());
    for (auto const c : s)
        out.push_back(cc::to_lower(c));
    return out;
}
} // namespace

uri uri::normalized() const
{
    auto const v = view();
    auto out = string();

    if (v.is_absolute())
    {
        out += to_lower_ascii(v.scheme());
        out.push_back(':');
    }

    if (v.has_authority())
    {
        out += "//";
        if (v.has_userinfo())
        {
            out += normalize_escapes(v.userinfo());
            out.push_back('@');
        }
        out += to_lower_ascii(normalize_escapes(v.host()));
        if (v.has_port())
        {
            out.push_back(':');
            out += v.port_text();
        }
    }

    out += remove_dot_segments(normalize_escapes(v.path()));

    if (v.has_query())
    {
        out.push_back('?');
        out += normalize_escapes(v.query());
    }
    if (v.has_fragment())
    {
        out.push_back('#');
        out += normalize_escapes(v.fragment());
    }

    // Normalization only ever rewrites within the grammar, so the result parses whenever the input did.
    auto normalized = uri::parse(out);
    CC_ASSERT(normalized.has_value(), "normalization produced text that does not parse");
    return cc::move(normalized.value());
}
} // namespace cc
