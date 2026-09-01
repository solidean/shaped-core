#include <babel-serializer/geometry/stl.hh>
#include <clean-core/common/endian.hh> // cc::load_bytes_le
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format.hh>

#include <charconv> // std::from_chars

// STL, in both containers.
//
// Binary is a fixed-stride record array, so it is read by offset rather than parsed.
// Ascii is line-oriented and deliberately lenient: the structural keywords (`outer loop`, `endloop`, `endfacet`,
// `endsolid`) are skipped rather than checked, since nothing downstream depends on them and real files disagree about
// which are present.
// What IS checked is that a facet carries exactly three vertices, because that is the one thing a triangle needs.

namespace babel::impl
{
namespace
{
constexpr isize stl_header_size = 80;
constexpr isize stl_binary_stride = 50; // normal + 3 positions, as 12 f32, plus a u16 attribute count

/// The triangle count a binary file of this size would declare, or -1 when the bytes cannot be one.
[[nodiscard]] i64 binary_triangle_count(cc::span<byte const> bytes)
{
    if (bytes.size() < stl_header_size + 4)
        return -1;

    auto const declared = i64(cc::load_bytes_le<u32>(bytes, stl_header_size));
    return stl_header_size + 4 + declared * stl_binary_stride == bytes.size() ? declared : -1;
}

/// Whether the bytes are a binary file with its LAST record cut short.
///
/// Every record before the last one fits exactly and only the final one runs off the end, which no ascii file reaches
/// by accident: its bytes 80..84 are text, so the count they read as puts the required size astronomically far from
/// the actual one.
/// Without this a truncated binary file falls through to the ascii parse and comes back as an empty `solid`, which is
/// the one failure mode nobody would think to check for.
[[nodiscard]] bool is_truncated_binary(cc::span<byte const> bytes)
{
    if (bytes.size() < stl_header_size + 4)
        return false;

    auto const declared = i64(cc::load_bytes_le<u32>(bytes, stl_header_size));
    if (declared <= 0)
        return false;

    auto const required = stl_header_size + 4 + declared * stl_binary_stride;
    return required > bytes.size() && stl_header_size + 4 + (declared - 1) * stl_binary_stride <= bytes.size();
}

/// A minimal whitespace tokenizer over one line, the same shape obj's parser uses.
struct line_tokenizer
{
    char const* p = nullptr;
    char const* end = nullptr;

    explicit line_tokenizer(cc::string_view s) : p(s.data()), end(s.data() + s.size()) {}

    [[nodiscard]] bool next(cc::string_view& out)
    {
        while (p < end && cc::is_space(*p))
            ++p;
        if (p == end)
            return false;
        auto const* const start = p;
        while (p < end && !cc::is_space(*p))
            ++p;
        out = cc::string_view(start, isize(p - start));
        return true;
    }

    /// The remaining text, trimmed — a `solid` name may contain spaces.
    [[nodiscard]] cc::string_view rest()
    {
        while (p < end && cc::is_space(*p))
            ++p;
        auto const* stop = end;
        while (stop > p && cc::is_space(*(stop - 1)))
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

/// Three floats off `tok`, which must be the rest of the line.
[[nodiscard]] bool parse_vec3(line_tokenizer& tok, f32& x, f32& y, f32& z)
{
    auto a = cc::string_view();
    auto b = cc::string_view();
    auto c = cc::string_view();
    return tok.next(a) && tok.next(b) && tok.next(c) && parse_float(a, x) && parse_float(b, y) && parse_float(c, z);
}

[[nodiscard]] cc::result<stl::data> read_binary(cc::span<byte const> bytes, i64 count)
{
    auto out = stl::data{.source = stl::container::binary};
    out.normals.reserve(isize(count));
    out.positions.reserve(isize(count) * 3);
    out.attribute_counts.reserve(isize(count));

    for (auto t = i64(0); t < count; ++t)
    {
        auto const base = stl_header_size + 4 + t * stl_binary_stride;
        out.normals.push_back(tg::vec3f(cc::load_bytes_le<f32>(bytes, base), cc::load_bytes_le<f32>(bytes, base + 4),
                                        cc::load_bytes_le<f32>(bytes, base + 8)));

        for (auto v = 0; v < 3; ++v)
        {
            auto const p = base + 12 + v * 12;
            out.positions.push_back(tg::pos3f(cc::load_bytes_le<f32>(bytes, p), cc::load_bytes_le<f32>(bytes, p + 4),
                                              cc::load_bytes_le<f32>(bytes, p + 8)));
        }

        out.attribute_counts.push_back(cc::load_bytes_le<u16>(bytes, base + 48));
    }

    return cc::move(out);
}

[[nodiscard]] cc::result<stl::data> read_ascii(cc::span<byte const> bytes)
{
    auto out = stl::data{.source = stl::container::ascii};

    auto const text = cc::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    auto line_number = i64(0);
    auto vertices_in_facet = 0;
    auto in_facet = false;

    auto begin = isize(0);
    while (begin <= text.size())
    {
        auto stop = begin;
        while (stop < text.size() && text[stop] != '\n')
            ++stop;

        auto const line = text.subview({.offset = begin, .size = stop - begin});
        begin = stop + 1;
        ++line_number;

        auto tok = line_tokenizer(line);
        auto keyword = cc::string_view();
        if (!tok.next(keyword))
        {
            if (stop >= text.size())
                break;
            continue;
        }

        if (keyword == "solid")
            out.name = cc::string(tok.rest());
        else if (keyword == "facet")
        {
            // A facet opening inside one is what makes the vertex-count check below skippable: it would reset the
            // count and keep the normal, so `normals` and `positions` stop describing the same triangles and nothing
            // downstream could tell.
            if (in_facet)
                return cc::error(
                    cc::format("stl: a facet opens on line {} while the previous one is still open", line_number));

            // `facet normal nx ny nz`; a facet with no normal keyword is malformed rather than normal-less.
            auto normal_keyword = cc::string_view();
            auto x = 0.0f;
            auto y = 0.0f;
            auto z = 0.0f;
            if (!tok.next(normal_keyword) || normal_keyword != "normal" || !parse_vec3(tok, x, y, z))
                return cc::error(cc::format("stl: malformed facet on line {}", line_number));

            out.normals.push_back(tg::vec3f(x, y, z));
            in_facet = true;
            vertices_in_facet = 0;
        }
        else if (keyword == "vertex")
        {
            auto x = 0.0f;
            auto y = 0.0f;
            auto z = 0.0f;
            if (!in_facet || !parse_vec3(tok, x, y, z))
                return cc::error(cc::format("stl: malformed vertex on line {}", line_number));

            ++vertices_in_facet;
            if (vertices_in_facet > 3)
                return cc::error(cc::format("stl: facet has more than three vertices, on line {}", line_number));
            out.positions.push_back(tg::pos3f(x, y, z));
        }
        else if (keyword == "endfacet")
        {
            if (vertices_in_facet != 3)
                return cc::error(cc::format("stl: facet ending on line {} has {} vertices, not three", line_number,
                                            vertices_in_facet));
            in_facet = false;
        }
        // outer / endloop / endsolid and anything else are skipped

        if (stop >= text.size())
            break;
    }

    if (in_facet)
        return cc::error("stl: the file ends inside a facet");

    // The pairing `data` promises — one normal per triangle, three positions per triangle — asserted once rather than
    // trusted to the branches above, since every future addition to this parser has to keep it too.
    if (out.positions.size() != out.normals.size() * 3)
        return cc::error(cc::format("stl: {} facets carry {} vertices, which is not three each", out.normals.size(),
                                    out.positions.size()));

    return cc::move(out);
}
} // namespace
} // namespace babel::impl

namespace babel::stl
{
container detect_container(cc::span<byte const> bytes)
{
    return babel::impl::binary_triangle_count(bytes) >= 0 ? container::binary : container::ascii;
}

cc::result<data> read(cc::span<byte const> bytes)
{
    CC_RECORD_SCOPE("stl.read");

    auto const count = babel::impl::binary_triangle_count(bytes);
    if (count >= 0)
        return babel::impl::read_binary(bytes, count);

    if (babel::impl::is_truncated_binary(bytes))
        return cc::error(cc::format("stl: the binary file is truncated — it declares more triangles than its {} bytes "
                                    "can hold",
                                    bytes.size()));

    return babel::impl::read_ascii(bytes);
}

cc::result<data> read(cc::string_view text)
{
    return read(cc::span<byte const>(reinterpret_cast<byte const*>(text.data()), text.size()));
}

cc::result<data> read(cc::read_stream& in)
{
    auto bytes = in.read_all();
    CC_RETURN_IF_ERROR(bytes);
    return read(cc::span<byte const>(bytes.value()));
}
} // namespace babel::stl
