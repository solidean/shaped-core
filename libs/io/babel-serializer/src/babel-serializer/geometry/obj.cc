#include <babel-serializer/geometry/obj.hh>
#include <clean-core/common/utility.hh> // cc::unit, cc::move
#include <clean-core/streams/span_stream.hh>
#include <clean-core/string/format.hh>

#include <charconv> // std::from_chars

// Line-oriented OBJ parser. One line at a time via read_stream::read_line — never buffers the whole file.
// Grouping directives (o / g / usemtl) open a face span that stays open until the next directive of the same
// kind (or end of file); the span records which faces it covers.

namespace babel::impl
{
namespace
{
// A minimal whitespace tokenizer over one line. '\r' is treated as blank so a stray CR (already mostly handled
// by read_line) never sticks to a token.
struct line_tokenizer
{
    char const* p = nullptr;
    char const* end = nullptr;

    explicit line_tokenizer(cc::string_view s) : p(s.data()), end(s.data() + s.size()) {}

    static bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

    [[nodiscard]] bool next(cc::string_view& out)
    {
        while (p < end && is_blank(*p))
            ++p;
        if (p == end)
            return false;
        auto const* const start = p;
        while (p < end && !is_blank(*p))
            ++p;
        out = cc::string_view(start, isize(p - start));
        return true;
    }

    /// The remaining text (trimmed of surrounding blanks) — used for `o` / `g` names that may contain spaces.
    [[nodiscard]] cc::string_view rest()
    {
        while (p < end && is_blank(*p))
            ++p;
        auto const* stop = end;
        while (stop > p && is_blank(*(stop - 1)))
            --stop;
        return cc::string_view(p, isize(stop - p));
    }
};

[[nodiscard]] bool parse_float(cc::string_view tok, f32& out)
{
    auto const* const first = tok.data();
    auto const* const last = tok.data() + tok.size();
    auto const [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc() && ptr == last;
}

[[nodiscard]] bool parse_int(cc::string_view tok, i32& out)
{
    auto const* const first = tok.data();
    auto const* const last = tok.data() + tok.size();
    auto const [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc() && ptr == last;
}

// resolve an OBJ index against the current attribute count: 1-based positive, negative = relative from the end.
[[nodiscard]] i32 resolve_index(i32 raw, i32 count)
{
    if (raw > 0)
        return raw - 1;
    if (raw < 0)
        return count + raw; // -1 addresses the last element
    return -1;              // 0 never appears in valid OBJ
}

struct obj_parser
{
    obj::data result;
    int line_number = 0;

    // currently-open grouping spans (index into the matching vector, or -1)
    i32 open_object = -1;
    i32 open_group = -1;
    i32 open_material = -1;

    [[nodiscard]] auto error_here(cc::string_view what)
    {
        return cc::error(cc::format("OBJ parse error on line {}: {}", line_number, what));
    }

    void close_span(cc::vector<obj::group>& spans, i32 open)
    {
        if (open >= 0)
            spans[isize(open)].face_count = i32(result.faces.size()) - spans[isize(open)].first_face;
    }

    void open_span(cc::vector<obj::group>& spans, i32& open, cc::string_view name)
    {
        close_span(spans, open);
        open = i32(spans.size());
        spans.push_back({.name = cc::string(name), .first_face = i32(result.faces.size()), .face_count = 0});
    }

    [[nodiscard]] bool parse_corner(cc::string_view tok, obj::corner& out)
    {
        // split "v", "v/vt", "v//vn" or "v/vt/vn" on '/'
        cc::string_view parts[3] = {};
        auto part_count = 0;
        auto const* const e = tok.data() + tok.size();
        auto const* s = tok.data();
        for (auto const* p = tok.data(); p <= e; ++p)
        {
            if (p == e || *p == '/')
            {
                if (part_count < 3)
                    parts[part_count] = cc::string_view(s, isize(p - s));
                ++part_count;
                s = p + 1;
            }
        }

        auto vi = i32(0);
        if (parts[0].empty() || !parse_int(parts[0], vi)) // the position index is mandatory
            return false;
        out.position = resolve_index(vi, i32(result.positions.size()));

        if (part_count >= 2 && !parts[1].empty())
        {
            auto ti = i32(0);
            if (!parse_int(parts[1], ti))
                return false;
            out.texcoord = resolve_index(ti, i32(result.texcoords.size()));
        }
        if (part_count >= 3 && !parts[2].empty())
        {
            auto ni = i32(0);
            if (!parse_int(parts[2], ni))
                return false;
            out.normal = resolve_index(ni, i32(result.normals.size()));
        }
        return true;
    }

    [[nodiscard]] cc::result<cc::unit> parse_line(cc::string_view line)
    {
        auto tok = line_tokenizer(line);
        auto keyword = cc::string_view();
        if (!tok.next(keyword))
            return cc::unit{}; // blank line
        if (keyword.front() == '#')
            return cc::unit{}; // comment

        if (keyword == "v")
        {
            auto xyz = tg::pos3f();
            for (auto i = 0; i < 3; ++i)
            {
                auto value = cc::string_view();
                if (!tok.next(value) || !parse_float(value, xyz[i]))
                    return error_here("expected 3 coordinates after 'v'");
            }
            result.positions.push_back(xyz);
        }
        else if (keyword == "vt")
        {
            auto uv = tg::vec2f(0, 0);
            auto u = cc::string_view();
            if (!tok.next(u) || !parse_float(u, uv[0]))
                return error_here("expected at least a u coordinate after 'vt'");
            auto v = cc::string_view();
            if (tok.next(v) && !parse_float(v, uv[1]))
                return error_here("invalid v coordinate after 'vt'");
            result.texcoords.push_back(uv);
        }
        else if (keyword == "vn")
        {
            auto n = tg::vec3f();
            for (auto i = 0; i < 3; ++i)
            {
                auto value = cc::string_view();
                if (!tok.next(value) || !parse_float(value, n[i]))
                    return error_here("expected 3 components after 'vn'");
            }
            result.normals.push_back(n);
        }
        else if (keyword == "f")
        {
            auto const first_corner = i32(result.corners.size());
            auto vertex = cc::string_view();
            while (tok.next(vertex))
            {
                auto c = obj::corner();
                if (!parse_corner(vertex, c))
                    return error_here("malformed face corner");
                result.corners.push_back(c);
            }
            auto const count = i32(result.corners.size()) - first_corner;
            if (count == 0)
                return error_here("face has no corners");
            result.faces.push_back({.first_corner = first_corner, .corner_count = count});
        }
        else if (keyword == "o")
            open_span(result.objects, open_object, tok.rest());
        else if (keyword == "g")
            open_span(result.groups, open_group, tok.rest());
        else if (keyword == "usemtl")
            open_span(result.materials, open_material, tok.rest());
        else if (keyword == "mtllib")
        {
            auto name = cc::string_view();
            while (tok.next(name))
                result.material_libraries.push_back(cc::string(name));
        }
        // any other directive (s, vp, curve data, ...) is skipped
        return cc::unit{};
    }

    [[nodiscard]] cc::result<obj::data> parse(cc::read_stream& in)
    {
        auto line = cc::string();
        while (true)
        {
            auto more = in.read_line(line);
            CC_RETURN_IF_ERROR(more);
            if (!more.value())
                break;
            ++line_number;
            CC_RETURN_IF_ERROR(parse_line(line));
        }

        // close any still-open grouping spans against the final face count
        close_span(result.objects, open_object);
        close_span(result.groups, open_group);
        close_span(result.materials, open_material);

        return cc::move(result);
    }
};
} // namespace
} // namespace babel::impl

namespace babel::obj
{
cc::result<data> read(cc::read_stream& in)
{
    auto parser = babel::impl::obj_parser();
    return parser.parse(in);
}

cc::result<data> read(cc::span<cc::byte const> bytes)
{
    auto adapter = cc::span_read_stream_adapter(bytes);
    cc::read_stream stream = adapter;
    return read(stream);
}

cc::result<data> read(cc::string_view text)
{
    return read(cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(text.data()), text.size()));
}
} // namespace babel::obj
