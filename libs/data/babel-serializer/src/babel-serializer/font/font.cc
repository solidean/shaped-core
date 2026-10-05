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

/// The size of a `GPOS` value record of `format`: two bytes per field the format's bits name.
[[nodiscard]] isize value_record_size(u16 format)
{
    auto n = isize(0);
    for (auto bit = 0; bit < 8; ++bit)
        if ((format >> bit) & 1)
            ++n;
    return n * 2;
}

/// The X advance a value record of `format` at `at` carries, or 0 when the format has none.
[[nodiscard]] i32 x_advance_of(cc::span<byte const> table, isize at, u16 format)
{
    if ((format & 0x0004) == 0)
        return 0;
    auto r = be_reader{.bytes = table, .pos = at + value_record_size(u16(format & 0x0003))};
    auto const v = r.read<i16>();
    return r.ok ? i32(v) : 0;
}

/// The coverage index of `g` in the coverage table at `at`, or -1 when it is not covered.
[[nodiscard]] i32 coverage_index(cc::span<byte const> table, isize at, u16 g)
{
    auto r = be_reader{.bytes = table, .pos = at};
    auto const format = r.read<u16>();
    auto const count = isize(r.read<u16>());
    if (!r.ok)
        return -1;

    // Both forms are sorted by glyph, so the search is binary.
    auto lo = isize(0);
    auto hi = count - 1;
    while (lo <= hi)
    {
        auto const mid = (lo + hi) / 2;
        if (format == 1)
        {
            auto e = be_reader{.bytes = table, .pos = at + 4 + mid * 2};
            auto const glyph = e.read<u16>();
            if (!e.ok)
                return -1;
            if (glyph == g)
                return i32(mid);
            if (glyph < g)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        else if (format == 2)
        {
            auto e = be_reader{.bytes = table, .pos = at + 4 + mid * 6};
            auto const start = e.read<u16>();
            auto const end = e.read<u16>();
            auto const start_index = e.read<u16>();
            if (!e.ok)
                return -1;
            if (g < start)
                hi = mid - 1;
            else if (g > end)
                lo = mid + 1;
            else
                return i32(start_index) + i32(g - start);
        }
        else
            return -1;
    }
    return -1;
}

/// The class the class definition at `at` assigns `g`; 0, the default class, for a glyph it does not list.
[[nodiscard]] i32 class_of(cc::span<byte const> table, isize at, u16 g)
{
    auto r = be_reader{.bytes = table, .pos = at};
    auto const format = r.read<u16>();
    if (format == 1)
    {
        auto const start = r.read<u16>();
        auto const count = r.read<u16>();
        if (!r.ok || g < start || g >= u32(start) + count)
            return 0;
        r.pos = at + 6 + isize(g - start) * 2;
        auto const c = r.read<u16>();
        return r.ok ? i32(c) : 0;
    }
    if (format == 2)
    {
        auto const count = isize(r.read<u16>());
        if (!r.ok)
            return 0;
        auto lo = isize(0);
        auto hi = count - 1;
        while (lo <= hi)
        {
            auto const mid = (lo + hi) / 2;
            auto e = be_reader{.bytes = table, .pos = at + 4 + mid * 6};
            auto const start = e.read<u16>();
            auto const end = e.read<u16>();
            auto const c = e.read<u16>();
            if (!e.ok)
                return 0;
            if (g < start)
                hi = mid - 1;
            else if (g > end)
                lo = mid + 1;
            else
                return i32(c);
        }
    }
    return 0;
}

/// The X-advance adjustment pair-positioning subtable `sub` makes to `left` before `right`, or nullopt when it does not
/// cover the pair — which is what tells the caller to try the lookup's next subtable.
[[nodiscard]] cc::optional<i32> pair_adjustment(cc::span<byte const> sub, u16 left, u16 right)
{
    auto r = be_reader{.bytes = sub};
    auto const format = r.read<u16>();
    auto const coverage = isize(r.read<u16>());
    auto const format1 = r.read<u16>();
    auto const format2 = r.read<u16>();
    if (!r.ok)
        return {};
    auto const index = coverage_index(sub, coverage, left);
    if (index < 0)
        return {};
    auto const size1 = value_record_size(format1);
    auto const size2 = value_record_size(format2);

    if (format == 1)
    {
        auto const set_count = r.read<u16>();
        if (!r.ok || index >= set_count)
            return {};
        r.pos = 10 + isize(index) * 2;
        auto const set = isize(r.read<u16>());
        auto p = be_reader{.bytes = sub, .pos = set};
        auto const count = isize(p.read<u16>());
        if (!p.ok)
            return {};
        auto const record = 2 + size1 + size2;

        // Sorted by the second glyph.
        auto lo = isize(0);
        auto hi = count - 1;
        while (lo <= hi)
        {
            auto const mid = (lo + hi) / 2;
            auto const at = set + 2 + mid * record;
            auto e = be_reader{.bytes = sub, .pos = at};
            auto const second = e.read<u16>();
            if (!e.ok)
                return {};
            if (second == right)
                return x_advance_of(sub, at + 2, format1);
            if (second < right)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        return {};
    }
    if (format == 2)
    {
        auto const class_def1 = isize(r.read<u16>());
        auto const class_def2 = isize(r.read<u16>());
        auto const class1_count = r.read<u16>();
        auto const class2_count = r.read<u16>();
        if (!r.ok)
            return {};
        auto const c1 = class_of(sub, class_def1, left);
        auto const c2 = class_of(sub, class_def2, right);
        if (c1 >= class1_count || c2 >= class2_count)
            return {};
        auto const at = 16 + (isize(c1) * class2_count + c2) * (size1 + size2);
        return x_advance_of(sub, at, format1);
    }
    return {};
}

/// The FeatureList indices one language system of `gpos` registers: the script `DFLT`, else `latn`, else the first,
/// and that script's default LangSys, else its first.
/// Nullopt when the ScriptList names no script, which is the fallback to every feature.
[[nodiscard]] cc::optional<cc::vector<u16>> language_system_features(cc::span<byte const> gpos)
{
    auto r = be_reader{.bytes = gpos, .pos = 4};
    auto const scripts = isize(r.read<u16>());
    auto s = be_reader{.bytes = gpos, .pos = scripts};
    auto const script_count = s.read<u16>();
    if (!r.ok || !s.ok || script_count == 0)
        return {};

    auto script = isize(-1);
    auto best = 3;
    for (auto i = 0; i < script_count; ++i)
    {
        auto e = be_reader{.bytes = gpos, .pos = scripts + 2 + isize(i) * 6};
        auto const tag = e.read<u32>();
        auto const offset = isize(e.read<u16>());
        if (!e.ok)
            break;
        auto const rank = tag == tag_of("DFLT") ? 0 : tag == tag_of("latn") ? 1 : 2;
        if (rank < best)
        {
            best = rank;
            script = scripts + offset;
        }
    }
    if (script < 0)
        return {};

    auto out = cc::vector<u16>();
    auto t = be_reader{.bytes = gpos, .pos = script};
    auto lang_sys = isize(t.read<u16>());
    auto const lang_sys_count = t.read<u16>();
    if (lang_sys == 0 && lang_sys_count > 0)
    {
        t.skip(4); // the first LangSysRecord's tag
        lang_sys = isize(t.read<u16>());
    }
    if (!t.ok || lang_sys == 0)
        return out;

    auto l = be_reader{.bytes = gpos, .pos = script + lang_sys};
    l.skip(2); // lookupOrderOffset, reserved
    auto const required = l.read<u16>();
    auto const count = l.read<u16>();
    if (l.ok && required != 0xFFFF)
        out.push_back(required);
    for (auto i = 0; i < count && l.ok; ++i)
    {
        auto const index = l.read<u16>();
        if (l.ok)
            out.push_back(index);
    }
    return out;
}

/// Calls `add(subtable, lookup)` for the pair-adjustment subtables of every lookup one language system's `kern`
/// features name, in lookup order, extensions resolved.
/// Anything malformed is skipped rather than reported: kerning refines spacing, and a face without it still reads.
template <class F>
void for_each_kern_pair_subtable(cc::span<byte const> gpos, F&& add)
{
    auto r = be_reader{.bytes = gpos, .pos = 6};
    auto const features = isize(r.read<u16>());
    auto const lookups = isize(r.read<u16>());
    if (!r.ok)
        return;

    // Every lookup a chosen `kern` feature lists, once each and in lookup order, which is the order they apply in.
    auto r_features = be_reader{.bytes = gpos, .pos = features};
    auto const feature_count = r_features.read<u16>();
    auto r_lookups = be_reader{.bytes = gpos, .pos = lookups};
    auto const lookup_count = r_lookups.read<u16>();
    if (!r_features.ok || !r_lookups.ok)
        return;
    auto wanted = cc::vector<bool>::create_defaulted(isize(lookup_count));
    auto const take = [&](isize i)
    {
        auto f = be_reader{.bytes = gpos, .pos = features + 2 + i * 6};
        auto const tag = f.read<u32>();
        auto const offset = isize(f.read<u16>());
        if (!f.ok || tag != tag_of("kern"))
            return;
        auto l = be_reader{.bytes = gpos, .pos = features + offset + 2};
        auto const count = l.read<u16>();
        for (auto k = 0; k < count && l.ok; ++k)
        {
            auto const index = l.read<u16>();
            if (l.ok && index < lookup_count)
                wanted[index] = true;
        }
    };
    if (auto const chosen = language_system_features(gpos); chosen.has_value())
    {
        for (auto const i : chosen.value())
            if (i < feature_count)
                take(isize(i));
    }
    else
        for (auto i = isize(0); i < feature_count; ++i)
            take(i);

    for (auto i = isize(0); i < lookup_count; ++i)
    {
        if (!wanted[i])
            continue;
        auto e = be_reader{.bytes = gpos, .pos = lookups + 2 + i * 2};
        auto const lookup = lookups + isize(e.read<u16>());
        auto h = be_reader{.bytes = gpos, .pos = lookup};
        auto const type = h.read<u16>();
        h.skip(2); // lookupFlag
        auto const sub_count = h.read<u16>();
        if (!e.ok || !h.ok || (type != 2 && type != 9))
            continue;
        for (auto k = 0; k < sub_count; ++k)
        {
            auto o = be_reader{.bytes = gpos, .pos = lookup + 6 + isize(k) * 2};
            auto sub = lookup + isize(o.read<u16>());
            if (!o.ok)
                break;
            if (type == 9)
            {
                // An extension: format 1, the lookup type it wraps, and a 32-bit offset from itself to the real subtable.
                auto x = be_reader{.bytes = gpos, .pos = sub};
                x.skip(2);
                auto const wrapped = x.read<u16>();
                auto const offset = isize(x.read<u32>());
                if (!x.ok || wrapped != 2)
                    continue;
                sub += offset;
            }
            if (sub >= 0 && sub < gpos.size())
                add(gpos.subspan(sub), i32(i));
        }
    }
}

/// The value of the pair `key` among the `count` format-0 pairs at `at`, sorted by the pair; nullopt when it is not one.
[[nodiscard]] cc::optional<i32> legacy_pair_value(cc::span<byte const> kern, isize at, isize count, u32 key)
{
    auto lo = isize(0);
    auto hi = count - 1;
    while (lo <= hi)
    {
        auto const mid = (lo + hi) / 2;
        auto e = be_reader{.bytes = kern, .pos = at + mid * 6};
        auto const pair = e.read<u32>();
        auto const value = e.read<i16>();
        if (!e.ok)
            return {};
        if (pair == key)
            return i32(value);
        if (pair < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return {};
}

/// The legacy `kern` table's adjustment for the pair, summed over its horizontal format-0 subtables.
/// Version 0 is Microsoft's, where a subtable with the override bit that holds the pair replaces the sum so far.
/// Version 1 is Apple's: a header of its own, a coverage byte that means something else, and no override.
[[nodiscard]] i32 legacy_kerning(cc::span<byte const> kern, u16 left, u16 right)
{
    auto const key = (u32(left) << 16) | right;
    auto r = be_reader{.bytes = kern};
    auto const major = r.read<u16>();
    auto const is_apple = major == 1;
    auto tables = isize(0);
    if (is_apple)
    {
        r.skip(2); // the version's low half
        tables = isize(r.read<u32>());
    }
    else
        tables = isize(r.read<u16>());
    if (!r.ok || major > 1)
        return 0;

    auto total = 0;
    auto at = r.pos;
    for (auto t = isize(0); t < tables; ++t)
    {
        auto h = be_reader{.bytes = kern, .pos = at};
        auto length = isize(0);
        auto applies = false;
        auto is_override = false;
        if (is_apple)
        {
            length = isize(h.read<u32>());
            auto const coverage = h.read<u8>();
            auto const format = h.read<u8>();
            h.skip(2); // tupleIndex
            // Neither vertical (0x80), cross-stream (0x40) nor a variation (0x20).
            applies = (coverage & 0xE0) == 0 && format == 0;
        }
        else
        {
            h.skip(2); // version
            length = isize(h.read<u16>());
            auto const coverage = h.read<u16>();
            // Horizontal (bit 0), format 0 (high byte), neither minimum nor cross-stream.
            applies = (coverage & 0x0007) == 0x0001 && (coverage >> 8) == 0;
            is_override = (coverage & 0x0008) != 0;
        }
        auto const pairs = isize(h.read<u16>());
        if (!h.ok || length <= 0)
            break;
        // The pairs follow nPairs, searchRange, entrySelector and rangeShift.
        if (applies)
            if (auto const v = legacy_pair_value(kern, h.pos + 6, pairs, key); v.has_value())
                total = is_override ? v.value() : total + v.value();
        at += length;
    }
    return total;
}

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
        {
            // u32 before the range check: a corrupt group past 0xFFFF must not wrap onto a real glyph.
            auto const glyph = cc::load_bytes_be<u32>(sub, at + 8) + (c - start);
            return glyph > 0xFFFF ? u16(0) : u16(glyph);
        }
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
        // Glyph 0 is `.notdef`, which every face must have: it is what a face draws for a character it has none for.
        if (f._metrics.glyph_count == 0)
            return cc::error("'maxp' states no glyphs, not even .notdef");
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
        if (f._long_metrics == 0)
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
            if (!c.ok)
                break;
            // a record pointing past the table is skipped, not the end of the scan: later records may be valid
            if (offset + 4 > cmap.value().size())
                continue;
            auto const format = cc::load_bytes_be<u16>(cmap.value(), offset);
            auto const preference = babel::impl::cmap_preference(platform, encoding, format);
            if (preference <= best)
                continue;
            best = preference;
            f._cmap_format = format;
            f._cmap_subtable = cmap.value().subspan(offset);
        }
    }

    // Kerning is optional, and a malformed table of it costs the face its kerning rather than the face itself.
    if (auto gpos = dir.find(tag_of("GPOS")); gpos.has_value() && !gpos.value().empty())
        babel::impl::for_each_kern_pair_subtable(gpos.value(), [&](cc::span<byte const> sub, i32 lookup)
                                                 { f._pair_subtables.push_back({.bytes = sub, .lookup = lookup}); });
    if (auto kern = dir.find(tag_of("kern")); kern.has_value())
        f._kern = kern.value();

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

i32 face::pair_kerning(glyph_id left, glyph_id right) const
{
    if (_pair_subtables.empty())
        return _kern.empty() ? 0 : babel::impl::legacy_kerning(_kern, u16(left), u16(right));

    // Within a lookup the first subtable covering the pair applies; separate lookups each apply, so they sum.
    auto total = 0;
    auto applied = -1;
    for (auto const& sub : _pair_subtables)
    {
        if (sub.lookup == applied)
            continue;
        if (auto const v = babel::impl::pair_adjustment(sub.bytes, u16(left), u16(right)); v.has_value())
        {
            total += v.value();
            applied = sub.lookup;
        }
    }
    return total;
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
