#pragma once

#include <babel-serializer/font/font.hh>
#include <clean-core/container/vector.hh>

// Fonts built byte by byte for tests, so every encoding a reader decodes is one the test chose on purpose:
// short and long coordinate deltas, repeated flags, composites with a 2x2 transform, and both loca widths.
// Header-only, so another library's tests can craft a font too: babel's `BABEL_TEST_DIR` is the include root.

namespace babel_test
{
using namespace cc::primitive_defines;

struct be_writer;  // big-endian bytes, appended
struct test_glyph; // a simple outline or a composite, as the test states it
struct test_font;  // the glyphs of a one-face font, two layout switches, and its kerning
struct test_kern_pair;
struct test_kern_classes;
} // namespace babel_test

struct babel_test::be_writer
{
    cc::vector<byte> bytes;

    void u8_(u8 v) { bytes.push_back(byte(v)); }
    void u16_(u16 v)
    {
        u8_(u8(v >> 8));
        u8_(u8(v & 0xFF));
    }
    void i16_(i16 v) { u16_(u16(v)); }
    void u32_(u32 v)
    {
        u16_(u16(v >> 16));
        u16_(u16(v & 0xFFFF));
    }
    void pad_to_4()
    {
        while (bytes.size() % 4 != 0)
            u8_(0);
    }
};

struct babel_test::test_glyph
{
    cc::vector<babel::font::glyf_point> points;
    cc::vector<i32> contour_ends;

    /// Written as given: a component that is not the last must carry MORE_COMPONENTS (0x0020) in its flags.
    cc::vector<babel::font::glyf_component> components;
};

namespace babel_test
{

/// One simple glyph record, encoded the way a font tool would: the shortest delta each coordinate allows, and runs of
/// equal flags folded with the repeat bit.
[[nodiscard]] inline cc::vector<byte> encode_simple(test_glyph const& g)
{
    auto w = be_writer();
    w.i16_(i16(g.contour_ends.size()));
    w.i16_(0);
    w.i16_(0);
    w.i16_(500);
    w.i16_(700);
    for (auto const e : g.contour_ends)
        w.u16_(u16(e));
    w.u16_(2); // two bytes of instructions, which the reader must skip
    w.u8_(0xB0);
    w.u8_(0x00);

    auto flags = cc::vector<u8>();
    auto xs = be_writer();
    auto ys = be_writer();
    auto prev = tg::pos2i(0, 0);
    for (auto const& p : g.points)
    {
        auto flag = u8(p.on_curve ? 0x01 : 0x00);
        auto const encode = [&](i32 d, u8 short_bit, u8 same_bit, be_writer& out)
        {
            if (d == 0)
                flag |= same_bit;
            else if (d > -256 && d < 256)
            {
                flag |= short_bit;
                if (d > 0)
                    flag |= same_bit;
                out.u8_(u8(d < 0 ? -d : d));
            }
            else
                out.i16_(i16(d));
        };
        encode(p.position[0] - prev[0], 0x02, 0x10, xs);
        encode(p.position[1] - prev[1], 0x04, 0x20, ys);
        flags.push_back(flag);
        prev = p.position;
    }

    for (auto i = isize(0); i < flags.size();)
    {
        auto run = isize(1);
        while (i + run < flags.size() && flags[i + run] == flags[i] && run < 256)
            ++run;
        if (run > 1)
        {
            w.u8_(flags[i] | 0x08);
            w.u8_(u8(run - 1));
        }
        else
            w.u8_(flags[i]);
        i += run;
    }
    w.bytes.push_back_range(xs.bytes);
    w.bytes.push_back_range(ys.bytes);
    return w.bytes;
}

[[nodiscard]] inline cc::vector<byte> encode_composite(test_glyph const& g)
{
    auto w = be_writer();
    w.i16_(-1);
    w.i16_(0);
    w.i16_(0);
    w.i16_(500);
    w.i16_(700);
    for (auto i = isize(0); i < g.components.size(); ++i)
    {
        auto const& c = g.components[i];
        w.u16_(c.flags);
        w.u16_(u16(c.glyph));
        if ((c.flags & 0x0001) != 0)
        {
            w.i16_(i16(c.arg1));
            w.i16_(i16(c.arg2));
        }
        else
        {
            w.u8_(u8(c.arg1));
            w.u8_(u8(c.arg2));
        }
        if ((c.flags & 0x0080) != 0)
        {
            w.i16_(i16(c.xx * 16384.0f));
            w.i16_(i16(c.xy * 16384.0f));
            w.i16_(i16(c.yx * 16384.0f));
            w.i16_(i16(c.yy * 16384.0f));
        }
    }
    return w.bytes;
}

} // namespace babel_test

/// One kerned pair of glyph indices, and the X advance adjustment it makes to the first.
struct babel_test::test_kern_pair
{
    u16 left = 0;
    u16 right = 0;
    i16 value = 0;
};

/// A class-based kerning rule: every glyph of `left` before every glyph of `right` adjusts by `value`.
struct babel_test::test_kern_classes
{
    cc::vector<u16> left;
    cc::vector<u16> right;
    i16 value = 0;
};

struct babel_test::test_font
{
    cc::vector<test_glyph> glyphs;
    bool long_loca = true;
    bool with_cmap = true;

    /// A `GPOS` `kern` feature's first lookup: a per-pair subtable of these, then a per-class subtable of `gpos_classes`.
    /// Both are written only when some kerning is given.
    cc::vector<test_kern_pair> gpos_pairs;
    cc::vector<test_kern_classes> gpos_classes;

    /// A second lookup of the same feature, per-pair, whose adjustments add to the first's.
    cc::vector<test_kern_pair> gpos_second_lookup;

    /// Writes every lookup as an extension lookup (type 9) wrapping its pair subtables.
    bool gpos_as_extension = false;

    /// The legacy `kern` table's format-0 pairs, sorted by the writer.
    cc::vector<test_kern_pair> legacy_kern;
};

namespace babel_test
{

/// A coverage table (format 1) of `glyphs`, sorted.
[[nodiscard]] inline cc::vector<byte> encode_coverage(cc::vector<u16> glyphs)
{
    for (auto i = isize(1); i < glyphs.size(); ++i)
        for (auto k = i; k > 0 && glyphs[k - 1] > glyphs[k]; --k)
            cc::swap(glyphs[k - 1], glyphs[k]);
    auto w = be_writer();
    w.u16_(1);
    w.u16_(u16(glyphs.size()));
    for (auto const g : glyphs)
        w.u16_(g);
    return w.bytes;
}

/// A pair-adjustment subtable, format 1: one pair set per left glyph, X advance of the first glyph only.
[[nodiscard]] inline cc::vector<byte> encode_pair_pairs(cc::vector<test_kern_pair> pairs)
{
    for (auto i = isize(1); i < pairs.size(); ++i)
        for (auto k = i; k > 0
                         && (pairs[k - 1].left > pairs[k].left
                             || (pairs[k - 1].left == pairs[k].left && pairs[k - 1].right > pairs[k].right));
             --k)
            cc::swap(pairs[k - 1], pairs[k]);
    auto lefts = cc::vector<u16>();
    for (auto const& p : pairs)
        if (lefts.empty() || lefts.back() != p.left)
            lefts.push_back(p.left);

    auto const header = isize(10 + lefts.size() * 2);
    auto sets = be_writer();
    auto set_offsets = cc::vector<isize>();
    for (auto const l : lefts)
    {
        set_offsets.push_back(header + sets.bytes.size());
        auto count = 0;
        for (auto const& p : pairs)
            count += p.left == l ? 1 : 0;
        sets.u16_(u16(count));
        for (auto const& p : pairs)
            if (p.left == l)
            {
                sets.u16_(p.right);
                sets.i16_(p.value);
            }
    }
    auto const coverage = header + sets.bytes.size();

    auto w = be_writer();
    w.u16_(1);
    w.u16_(u16(coverage));
    w.u16_(0x0004); // value format 1: X advance
    w.u16_(0);      // value format 2: nothing
    w.u16_(u16(lefts.size()));
    for (auto const o : set_offsets)
        w.u16_(u16(o));
    w.bytes.push_back_range(sets.bytes);
    w.bytes.push_back_range(encode_coverage(lefts));
    return w.bytes;
}

/// A class definition (format 2) putting each of `glyphs` in class 1, one range per glyph, sorted.
[[nodiscard]] inline cc::vector<byte> encode_class_one(cc::vector<u16> glyphs)
{
    for (auto i = isize(1); i < glyphs.size(); ++i)
        for (auto k = i; k > 0 && glyphs[k - 1] > glyphs[k]; --k)
            cc::swap(glyphs[k - 1], glyphs[k]);
    auto w = be_writer();
    w.u16_(2);
    w.u16_(u16(glyphs.size()));
    for (auto const g : glyphs)
    {
        w.u16_(g);
        w.u16_(g);
        w.u16_(1);
    }
    return w.bytes;
}

/// A pair-adjustment subtable, format 2: classes 0 and 1 on each side, the rule's value at (1, 1) and 0 elsewhere.
[[nodiscard]] inline cc::vector<byte> encode_pair_classes(test_kern_classes const& c)
{
    auto const records = isize(16);              // the header
    auto const class_def1 = records + 2 * 2 * 2; // four i16 records
    auto const class_def1_bytes = encode_class_one(c.left);
    auto const class_def2 = class_def1 + class_def1_bytes.size();
    auto const class_def2_bytes = encode_class_one(c.right);
    auto const coverage = class_def2 + class_def2_bytes.size();

    auto w = be_writer();
    w.u16_(2);
    w.u16_(u16(coverage));
    w.u16_(0x0004);
    w.u16_(0);
    w.u16_(u16(class_def1));
    w.u16_(u16(class_def2));
    w.u16_(2);
    w.u16_(2);
    w.i16_(0);
    w.i16_(0);
    w.i16_(0);
    w.i16_(c.value);
    w.bytes.push_back_range(class_def1_bytes);
    w.bytes.push_back_range(class_def2_bytes);
    w.bytes.push_back_range(encode_coverage(c.left));
    return w.bytes;
}

/// One lookup of pair-adjustment subtables, as type 2 or wrapped in type-9 extensions.
[[nodiscard]] inline cc::vector<byte> encode_pair_lookup(cc::vector<cc::vector<byte>> const& subtables, bool extension)
{
    auto const header = isize(6 + subtables.size() * 2);
    auto body = be_writer();
    auto offsets = cc::vector<isize>();
    for (auto const& sub : subtables)
    {
        offsets.push_back(header + body.bytes.size());
        if (extension)
        {
            body.u16_(1);
            body.u16_(2);
            body.u32_(8); // the wrapped subtable follows the 8-byte extension record
        }
        body.bytes.push_back_range(sub);
    }
    auto w = be_writer();
    w.u16_(extension ? 9 : 2);
    w.u16_(0);
    w.u16_(u16(subtables.size()));
    for (auto const o : offsets)
        w.u16_(u16(o));
    w.bytes.push_back_range(body.bytes);
    return w.bytes;
}

/// A `GPOS` table of one `kern` feature over `lookups`, with an empty script list.
[[nodiscard]] inline cc::vector<byte> encode_gpos(cc::vector<cc::vector<byte>> const& lookups)
{
    auto const script_list = isize(10);
    auto const feature_list = script_list + 2;
    auto const feature_list_size = isize(2 + 6 + 4 + lookups.size() * 2);
    auto const lookup_list = feature_list + feature_list_size;

    auto w = be_writer();
    w.u16_(1);
    w.u16_(0);
    w.u16_(u16(script_list));
    w.u16_(u16(feature_list));
    w.u16_(u16(lookup_list));
    w.u16_(0); // no scripts: the reader does not consult them
    w.u16_(1);
    for (auto const c : {'k', 'e', 'r', 'n'})
        w.u8_(u8(c));
    w.u16_(8); // the feature follows the list's one record
    w.u16_(0);
    w.u16_(u16(lookups.size()));
    for (auto i = isize(0); i < lookups.size(); ++i)
        w.u16_(u16(i));

    auto at = isize(2 + lookups.size() * 2);
    w.u16_(u16(lookups.size()));
    for (auto const& l : lookups)
    {
        w.u16_(u16(at));
        at += l.size();
    }
    for (auto const& l : lookups)
        w.bytes.push_back_range(l);
    return w.bytes;
}

/// A legacy `kern` table: version 0, one horizontal format-0 subtable of `pairs`.
[[nodiscard]] inline cc::vector<byte> encode_legacy_kern(cc::vector<test_kern_pair> pairs)
{
    auto const key = [](test_kern_pair const& p) { return (u32(p.left) << 16) | p.right; };
    for (auto i = isize(1); i < pairs.size(); ++i)
        for (auto k = i; k > 0 && key(pairs[k - 1]) > key(pairs[k]); --k)
            cc::swap(pairs[k - 1], pairs[k]);
    auto w = be_writer();
    w.u16_(0);
    w.u16_(1);
    w.u16_(0);
    w.u16_(u16(14 + pairs.size() * 6));
    w.u16_(0x0001);
    w.u16_(u16(pairs.size()));
    w.u16_(0); // searchRange, entrySelector and rangeShift: the reader searches without them
    w.u16_(0);
    w.u16_(0);
    for (auto const& p : pairs)
    {
        w.u16_(p.left);
        w.u16_(p.right);
        w.i16_(p.value);
    }
    return w.bytes;
}

/// A one-face font of `head`, `maxp`, `hhea`, `hmtx`, `cmap`, `loca` and `glyf`.
/// Characters 'A', 'B', ... map to glyphs 1, 2, ...; glyph 0 is whatever `glyphs[0]` is, an empty `.notdef` by convention.
/// Two long metrics, so every glyph from 2 on shares glyph 1's advance.
[[nodiscard]] inline cc::vector<byte> build_font(test_font const& f)
{
    auto const glyph_count = isize(f.glyphs.size());

    auto glyf = be_writer();
    auto offsets = cc::vector<isize>();
    for (auto const& g : f.glyphs)
    {
        offsets.push_back(glyf.bytes.size());
        if (!g.components.empty())
            glyf.bytes.push_back_range(encode_composite(g));
        else if (!g.points.empty())
            glyf.bytes.push_back_range(encode_simple(g));
        glyf.pad_to_4();
    }
    offsets.push_back(glyf.bytes.size());

    auto loca = be_writer();
    for (auto const o : offsets)
    {
        if (f.long_loca)
            loca.u32_(u32(o));
        else
            loca.u16_(u16(o / 2));
    }

    auto head = be_writer();
    for (auto i = 0; i < 18; ++i)
        head.u8_(0);
    head.u16_(1000); // units per em
    for (auto i = 0; i < 16; ++i)
        head.u8_(0);
    head.i16_(-50);
    head.i16_(-200);
    head.i16_(900);
    head.i16_(800);
    head.u16_(0);
    head.u16_(0);
    head.i16_(0);
    head.i16_(f.long_loca ? 1 : 0);
    head.i16_(0);

    auto maxp = be_writer();
    maxp.u32_(0x00005000);
    maxp.u16_(u16(glyph_count));

    auto hhea = be_writer();
    hhea.u32_(0x00010000);
    hhea.i16_(800);
    hhea.i16_(-200);
    hhea.i16_(90);
    for (auto i = 10; i < 34; ++i)
        hhea.u8_(0);
    hhea.u16_(2);

    auto hmtx = be_writer();
    hmtx.u16_(250);
    hmtx.i16_(0);
    hmtx.u16_(600);
    hmtx.i16_(10);
    for (auto i = isize(2); i < glyph_count; ++i)
        hmtx.i16_(i16(20 + i));

    // format 4: one segment 'A'.. mapped by delta, plus the mandatory 0xFFFF terminator
    auto cmap = be_writer();
    cmap.u16_(0);
    cmap.u16_(1);
    cmap.u16_(3);
    cmap.u16_(1);
    cmap.u32_(12);
    auto const last = u16('A' + glyph_count - 2);
    cmap.u16_(4);
    cmap.u16_(32);
    cmap.u16_(0);
    cmap.u16_(4); // segCountX2
    cmap.u16_(4);
    cmap.u16_(1);
    cmap.u16_(0);
    cmap.u16_(last);
    cmap.u16_(0xFFFF);
    cmap.u16_(0);
    cmap.u16_('A');
    cmap.u16_(0xFFFF);
    cmap.i16_(i16(1 - 'A'));
    cmap.i16_(1);
    cmap.u16_(0);
    cmap.u16_(0);

    struct table
    {
        char const* tag = nullptr;
        be_writer* data = nullptr;
    };
    auto tables = cc::vector<table>();
    tables.push_back({"head", &head});
    tables.push_back({"maxp", &maxp});
    tables.push_back({"hhea", &hhea});
    tables.push_back({"hmtx", &hmtx});
    if (f.with_cmap)
        tables.push_back({"cmap", &cmap});
    tables.push_back({"loca", &loca});
    tables.push_back({"glyf", &glyf});

    auto gpos = be_writer();
    if (!f.gpos_pairs.empty() || !f.gpos_classes.empty() || !f.gpos_second_lookup.empty())
    {
        auto first = cc::vector<cc::vector<byte>>();
        if (!f.gpos_pairs.empty())
            first.push_back(encode_pair_pairs(f.gpos_pairs));
        for (auto const& c : f.gpos_classes)
            first.push_back(encode_pair_classes(c));
        auto lookups = cc::vector<cc::vector<byte>>();
        lookups.push_back(encode_pair_lookup(first, f.gpos_as_extension));
        if (!f.gpos_second_lookup.empty())
        {
            auto second = cc::vector<cc::vector<byte>>();
            second.push_back(encode_pair_pairs(f.gpos_second_lookup));
            lookups.push_back(encode_pair_lookup(second, f.gpos_as_extension));
        }
        gpos.bytes = encode_gpos(lookups);
        tables.push_back({"GPOS", &gpos});
    }
    auto kern = be_writer();
    if (!f.legacy_kern.empty())
    {
        kern.bytes = encode_legacy_kern(f.legacy_kern);
        tables.push_back({"kern", &kern});
    }

    auto out = be_writer();
    out.u32_(0x00010000);
    out.u16_(u16(tables.size()));
    out.u16_(0);
    out.u16_(0);
    out.u16_(0);
    auto data_at = 12 + tables.size() * 16;
    for (auto const& t : tables)
    {
        for (auto k = 0; k < 4; ++k)
            out.u8_(u8(t.tag[k]));
        out.u32_(0);
        out.u32_(u32(data_at));
        out.u32_(u32(t.data->bytes.size()));
        data_at += (t.data->bytes.size() + 3) / 4 * 4;
    }
    for (auto const& t : tables)
    {
        out.bytes.push_back_range(t.data->bytes);
        out.pad_to_4();
    }
    return out.bytes;
}
} // namespace babel_test
