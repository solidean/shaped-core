#include <babel-serializer/font/font.hh>
#include <clean-core/common/endian.hh> // cc::load_bytes_be
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/string/format.hh>

// OpenType's table directory, `head`, `maxp`, `hhea`, `hmtx`, `cmap` and TrueType's `glyf` / `loca`.
//
// Every read is bounds-checked against the table it belongs to, because a font file is untrusted input:
// `cc::load_bytes_be` asserts on an out-of-range offset, so each one is guarded by a size check that turns into an
// error first.

namespace babel::impl
{
namespace
{
/// A big-endian cursor over one table, failing sticky: a read past the end returns zero and sets `ok` false.
/// Callers read a whole record, then check `ok` once.
struct be_reader
{
    cc::span<byte const> bytes;
    isize pos = 0;
    bool ok = true;

    [[nodiscard]] bool has(isize n) const { return pos >= 0 && n >= 0 && pos + n <= bytes.size(); }

    template <class T>
    [[nodiscard]] T read()
    {
        if (!ok || !has(isize(sizeof(T))))
        {
            ok = false;
            return T(0);
        }
        auto const v = cc::load_bytes_be<T>(bytes, pos);
        pos += isize(sizeof(T));
        return v;
    }

    void skip(isize n)
    {
        if (!ok || !has(n))
        {
            ok = false;
            return;
        }
        pos += n;
    }
};

[[nodiscard]] u32 tag_of(char const (&s)[5])
{
    return (u32(u8(s[0])) << 24) | (u32(u8(s[1])) << 16) | (u32(u8(s[2])) << 8) | u32(u8(s[3]));
}

/// Two's-complement 2.14 fixed point.
[[nodiscard]] f32 from_f2dot14(i16 v)
{
    return f32(v) / 16384.0f;
}

struct table_directory
{
    cc::span<byte const> file;
    isize directory = 0;
    i32 table_count = 0;

    /// The table's bytes, or an empty span when the face has no such table.
    /// A table running past the file is an error rather than absent.
    [[nodiscard]] cc::result<cc::span<byte const>> find(u32 tag) const
    {
        for (auto i = 0; i < table_count; ++i)
        {
            auto r = be_reader{.bytes = file, .pos = directory + 12 + isize(i) * 16};
            auto const t = r.read<u32>();
            r.skip(4); // checksum
            auto const offset = r.read<u32>();
            auto const length = r.read<u32>();
            if (!r.ok)
                return cc::error("the table directory runs past the end of the file");
            if (t != tag)
                continue;
            if (i64(offset) + i64(length) > file.size())
                return cc::error(cc::format("table {:08x} runs past the end of the file", tag));
            return file.subspan({.offset = isize(offset), .size = isize(length)});
        }
        return cc::span<byte const>();
    }
};

/// The subtable `cmap` should be read through, best first: full Unicode before the BMP, Windows before Unicode-platform.
[[nodiscard]] int cmap_preference(u16 platform, u16 encoding, u16 format)
{
    if (format == 12 && ((platform == 3 && encoding == 10) || platform == 0))
        return 4;
    if (format == 4 && ((platform == 3 && encoding == 1) || platform == 0))
        return 3;
    if (format == 4 && platform == 3 && encoding == 0)
        return 1; // the symbol encoding still maps its private-use range
    return 0;
}

[[nodiscard]] u16 lookup_format4(cc::span<byte const> sub, u32 c)
{
    if (c > 0xFFFF)
        return 0;
    auto r = be_reader{.bytes = sub, .pos = 6};
    auto const seg_count = isize(r.read<u16>() / 2);
    if (!r.ok || seg_count == 0)
        return 0;

    auto const ends = isize(14);
    auto const starts = ends + seg_count * 2 + 2;
    auto const deltas = starts + seg_count * 2;
    auto const range_offsets = deltas + seg_count * 2;
    if (range_offsets + seg_count * 2 > sub.size())
        return 0;

    // Segments are sorted by end code, so the first one ending at or past `c` is the only one that can hold it.
    for (auto s = isize(0); s < seg_count; ++s)
    {
        auto const end = cc::load_bytes_be<u16>(sub, ends + s * 2);
        if (end < c)
            continue;
        auto const start = cc::load_bytes_be<u16>(sub, starts + s * 2);
        if (start > c)
            return 0;
        auto const delta = cc::load_bytes_be<u16>(sub, deltas + s * 2);
        auto const range_offset = cc::load_bytes_be<u16>(sub, range_offsets + s * 2);
        if (range_offset == 0)
            return u16((c + delta) & 0xFFFF);

        auto const at = range_offsets + s * 2 + isize(range_offset) + isize(c - start) * 2;
        if (at < 0 || at + 2 > sub.size())
            return 0;
        auto const g = cc::load_bytes_be<u16>(sub, at);
        return g == 0 ? u16(0) : u16((g + delta) & 0xFFFF);
    }
    return 0;
}

[[nodiscard]] u16 lookup_format12(cc::span<byte const> sub, u32 c)
{
    auto r = be_reader{.bytes = sub, .pos = 12};
    auto const groups = isize(r.read<u32>());
    if (!r.ok || 16 + groups * 12 > sub.size())
        return 0;

    // Groups are sorted by start code.
    auto lo = isize(0);
    auto hi = groups;
    while (lo < hi)
    {
        auto const mid = (lo + hi) / 2;
        auto const at = 16 + mid * 12;
        auto const start = cc::load_bytes_be<u32>(sub, at);
        auto const end = cc::load_bytes_be<u32>(sub, at + 4);
        if (c < start)
            hi = mid;
        else if (c > end)
            lo = mid + 1;
        else
            return u16(cc::load_bytes_be<u32>(sub, at + 8) + (c - start));
    }
    return 0;
}
} // namespace
} // namespace babel::impl

namespace babel::font
{
using babel::impl::be_reader;
using babel::impl::tag_of;

cc::result<face> read(cc::pinned_data<byte const> bytes, i32 face_index)
{
    CC_RECORD_SCOPE("font.read");
    auto const file = cc::span<byte const>(bytes);

    // A collection is a header naming one table directory per face; a single font is that directory.
    auto directory = isize(0);
    {
        auto r = be_reader{.bytes = file};
        auto const tag = r.read<u32>();
        if (!r.ok)
            return cc::error("too short for a font file");
        if (tag == tag_of("ttcf"))
        {
            r.skip(4); // version
            auto const faces = r.read<u32>();
            if (!r.ok || face_index < 0 || u32(face_index) >= faces)
                return cc::error(cc::format("the collection has no face {}", face_index));
            r.skip(isize(face_index) * 4);
            directory = isize(r.read<u32>());
            if (!r.ok)
                return cc::error("the collection header runs past the end of the file");
        }
        else if (face_index != 0)
            return cc::error(cc::format("a single font has no face {}", face_index));
    }

    auto r = be_reader{.bytes = file, .pos = directory};
    auto const version = r.read<u32>();
    auto const table_count = r.read<u16>();
    if (!r.ok)
        return cc::error("the table directory runs past the end of the file");
    if (version != 0x00010000 && version != tag_of("true") && version != tag_of("OTTO"))
        return cc::error(cc::format("not an OpenType font (version {:08x})", version));

    auto const dir = babel::impl::table_directory{.file = file, .directory = directory, .table_count = table_count};
    auto const require = [&](char const(&tag)[5]) -> cc::result<cc::span<byte const>>
    {
        auto t = dir.find(tag_of(tag));
        CC_RETURN_IF_ERROR(t);
        if (t.value().empty())
            return cc::error(cc::format("the font has no '{}' table", tag));
        return t.value();
    };

    auto head = require("head");
    CC_RETURN_IF_ERROR(head);
    auto maxp = require("maxp");
    CC_RETURN_IF_ERROR(maxp);
    auto hhea = require("hhea");
    CC_RETURN_IF_ERROR(hhea);
    auto hmtx = require("hmtx");
    CC_RETURN_IF_ERROR(hmtx);
    auto cmap = require("cmap");
    CC_RETURN_IF_ERROR(cmap);

    auto f = face();
    f._bytes = cc::move(bytes);

    {
        auto h = be_reader{.bytes = head.value(), .pos = 18};
        f._metrics.units_per_em = h.read<u16>();
        h.skip(16); // created, modified
        auto const x_min = h.read<i16>();
        auto const y_min = h.read<i16>();
        auto const x_max = h.read<i16>();
        auto const y_max = h.read<i16>();
        h.skip(6); // macStyle, lowestRecPPEM, fontDirectionHint
        f._long_loca = h.read<i16>() != 0;
        if (!h.ok)
            return cc::error("'head' is too short");
        f._metrics.bounds = tg::aabb2i(tg::pos2i(x_min, y_min), tg::pos2i(x_max, y_max));
        if (f._metrics.units_per_em == 0)
            return cc::error("'head' states zero units per em");
    }
    {
        auto m = be_reader{.bytes = maxp.value(), .pos = 4};
        f._metrics.glyph_count = m.read<u16>();
        if (!m.ok)
            return cc::error("'maxp' is too short");
    }
    {
        auto h = be_reader{.bytes = hhea.value(), .pos = 4};
        f._metrics.ascender = h.read<i16>();
        f._metrics.descender = h.read<i16>();
        f._metrics.line_gap = h.read<i16>();
        h.pos = 34;
        f._long_metrics = h.read<u16>();
        if (!h.ok)
            return cc::error("'hhea' is too short");
        if (f._long_metrics == 0 && f._metrics.glyph_count > 0)
            return cc::error("'hhea' states no horizontal metrics");
        auto const needed = isize(f._long_metrics) * 4 + isize(cc::max(0, f._metrics.glyph_count - f._long_metrics)) * 2;
        if (hmtx.value().size() < needed)
            return cc::error("'hmtx' is shorter than 'hhea' and 'maxp' say");
        f._hmtx = hmtx.value();
    }
    {
        auto c = be_reader{.bytes = cmap.value(), .pos = 2};
        auto const subtables = c.read<u16>();
        auto best = 0;
        for (auto i = 0; i < subtables && c.ok; ++i)
        {
            auto const platform = c.read<u16>();
            auto const encoding = c.read<u16>();
            auto const offset = isize(c.read<u32>());
            if (!c.ok || offset + 4 > cmap.value().size())
                break;
            auto const format = cc::load_bytes_be<u16>(cmap.value(), offset);
            auto const preference = babel::impl::cmap_preference(platform, encoding, format);
            if (preference <= best)
                continue;
            best = preference;
            f._cmap_format = format;
            f._cmap_subtable = cmap.value().subspan(offset);
        }
    }

    auto glyf = dir.find(tag_of("glyf"));
    CC_RETURN_IF_ERROR(glyf);
    auto loca = dir.find(tag_of("loca"));
    CC_RETURN_IF_ERROR(loca);
    auto cff = dir.find(tag_of("CFF "));
    CC_RETURN_IF_ERROR(cff);
    auto cff2 = dir.find(tag_of("CFF2"));
    CC_RETURN_IF_ERROR(cff2);

    if (!glyf.value().empty() && !loca.value().empty())
    {
        auto const entry = f._long_loca ? isize(4) : isize(2);
        if (loca.value().size() < (isize(f._metrics.glyph_count) + 1) * entry)
            return cc::error("'loca' is shorter than 'maxp' says");
        f._glyf = glyf.value();
        f._loca = loca.value();
        f._outlines = outline_format::truetype;
    }
    else if (!cff.value().empty() || !cff2.value().empty())
        f._outlines = outline_format::cff;

    return f;
}

cc::result<face> read(cc::span<byte const> bytes, i32 face_index)
{
    // A span is a borrow, and the face keeps its bytes, so this pins an owned copy.
    return read(cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(bytes)), face_index);
}

cc::result<face> read(cc::read_stream& in, i32 face_index)
{
    auto slurped = in.read_all();
    CC_RETURN_IF_ERROR(slurped);
    return read(cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(slurped).value())), face_index);
}

cc::optional<glyph_id> face::glyph_for(char32_t codepoint) const
{
    auto const c = u32(codepoint);
    auto g = u16(0);
    if (_cmap_format == 4)
        g = babel::impl::lookup_format4(_cmap_subtable, c);
    else if (_cmap_format == 12)
        g = babel::impl::lookup_format12(_cmap_subtable, c);

    if (g == 0 || i32(g) >= _metrics.glyph_count)
        return cc::nullopt;
    return glyph_id(g);
}

horizontal_metric face::horizontal(glyph_id g) const
{
    auto const i = i32(g);
    CC_ASSERT(i < _metrics.glyph_count, "the glyph is not in this face");
    if (i < _long_metrics)
        return {.advance = cc::load_bytes_be<u16>(_hmtx, isize(i) * 4),
                .left_side_bearing = cc::load_bytes_be<i16>(_hmtx, isize(i) * 4 + 2)};

    auto const last_advance = cc::load_bytes_be<u16>(_hmtx, isize(_long_metrics - 1) * 4);
    auto const at = isize(_long_metrics) * 4 + isize(i - _long_metrics) * 2;
    return {.advance = last_advance, .left_side_bearing = cc::load_bytes_be<i16>(_hmtx, at)};
}

cc::result<glyf_outline> face::outline(glyph_id g) const
{
    auto const i = isize(g);
    CC_ASSERT(i < _metrics.glyph_count, "the glyph is not in this face");
    if (_outlines != outline_format::truetype)
        return cc::error("this face's outlines are not 'glyf'");

    auto const entry_at = [&](isize k)
    {
        return _long_loca ? isize(cc::load_bytes_be<u32>(_loca, k * 4)) : isize(cc::load_bytes_be<u16>(_loca, k * 2)) * 2;
    };
    auto const begin = entry_at(i);
    auto const end = entry_at(i + 1);

    auto out = glyf_outline();
    if (begin == end)
        return out; // no outline at all: a space
    if (begin > end || end > _glyf.size())
        return cc::error(cc::format("glyph {}'s record runs past 'glyf'", i));

    auto r = be_reader{.bytes = _glyf.subspan({.start = begin, .end = end})};
    auto const contour_count = r.read<i16>();
    auto const x_min = r.read<i16>();
    auto const y_min = r.read<i16>();
    auto const x_max = r.read<i16>();
    auto const y_max = r.read<i16>();
    if (!r.ok)
        return cc::error(cc::format("glyph {}'s header is cut short", i));
    out.bounds = tg::aabb2i(tg::pos2i(x_min, y_min), tg::pos2i(x_max, y_max));

    if (contour_count >= 0)
    {
        out.contour_ends.reserve(contour_count);
        auto point_count = isize(0);
        for (auto c = 0; c < contour_count; ++c)
        {
            auto const last = isize(r.read<u16>());
            if (r.ok && (last + 1 < point_count))
                return cc::error(cc::format("glyph {}'s contour ends are not ascending", i));
            point_count = last + 1;
            out.contour_ends.push_back(i32(last));
        }
        r.skip(isize(r.read<u16>())); // instructions: hinting, which nothing here interprets
        if (!r.ok)
            return cc::error(cc::format("glyph {}'s contours are cut short", i));

        // The flags, run-length encoded: bit 3 repeats a flag as many more times as the next byte says.
        auto flags = cc::vector<u8>::create_with_capacity(point_count);
        while (flags.size() < point_count && r.ok)
        {
            auto const flag = r.read<u8>();
            flags.push_back(flag);
            if ((flag & 0x08) != 0)
            {
                auto const repeats = isize(r.read<u8>());
                for (auto k = isize(0); k < repeats && flags.size() < point_count; ++k)
                    flags.push_back(flag);
            }
        }

        // Coordinates are deltas from the previous point, short or long, or omitted to repeat it.
        out.points.resize_to_defaulted(point_count);
        auto const read_axis = [&](u8 short_bit, u8 same_or_positive_bit, int axis)
        {
            auto value = i32(0);
            for (auto k = isize(0); k < point_count && r.ok; ++k)
            {
                auto const flag = flags[k];
                if ((flag & short_bit) != 0)
                {
                    auto const d = i32(r.read<u8>());
                    value += (flag & same_or_positive_bit) != 0 ? d : -d;
                }
                else if ((flag & same_or_positive_bit) == 0)
                    value += r.read<i16>();
                out.points[k].position[axis] = value;
            }
        };
        read_axis(0x02, 0x10, 0);
        read_axis(0x04, 0x20, 1);
        if (!r.ok)
            return cc::error(cc::format("glyph {}'s points are cut short", i));
        for (auto k = isize(0); k < point_count; ++k)
            out.points[k].on_curve = (flags[k] & 0x01) != 0;
        return out;
    }

    // A composite: components until one stops setting MORE_COMPONENTS.
    auto more = true;
    while (more)
    {
        auto c = glyf_component();
        c.flags = r.read<u16>();
        c.glyph = glyph_id(r.read<u16>());
        c.args_are_offset = (c.flags & 0x0002) != 0;
        if ((c.flags & 0x0001) != 0)
        {
            c.arg1 = c.args_are_offset ? i32(r.read<i16>()) : i32(r.read<u16>());
            c.arg2 = c.args_are_offset ? i32(r.read<i16>()) : i32(r.read<u16>());
        }
        else
        {
            c.arg1 = c.args_are_offset ? i32(r.read<i8>()) : i32(r.read<u8>());
            c.arg2 = c.args_are_offset ? i32(r.read<i8>()) : i32(r.read<u8>());
        }
        if ((c.flags & 0x0008) != 0)
        {
            c.xx = babel::impl::from_f2dot14(r.read<i16>());
            c.yy = c.xx;
        }
        else if ((c.flags & 0x0040) != 0)
        {
            c.xx = babel::impl::from_f2dot14(r.read<i16>());
            c.yy = babel::impl::from_f2dot14(r.read<i16>());
        }
        else if ((c.flags & 0x0080) != 0)
        {
            c.xx = babel::impl::from_f2dot14(r.read<i16>());
            c.xy = babel::impl::from_f2dot14(r.read<i16>());
            c.yx = babel::impl::from_f2dot14(r.read<i16>());
            c.yy = babel::impl::from_f2dot14(r.read<i16>());
        }
        if (!r.ok)
            return cc::error(cc::format("glyph {}'s components are cut short", i));
        if (isize(c.glyph) >= _metrics.glyph_count)
            return cc::error(cc::format("glyph {} names a component outside the face", i));
        out.components.push_back(c);
        more = (c.flags & 0x0020) != 0;
    }
    return out;
}
} // namespace babel::font
