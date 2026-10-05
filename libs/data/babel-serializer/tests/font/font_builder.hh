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
struct test_legacy_kern_subtable;
struct test_gpos_feature;
struct test_gpos_script;
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

/// One subtable of a legacy `kern` table: format 0, horizontal unless said otherwise.
struct babel_test::test_legacy_kern_subtable
{
    cc::vector<test_kern_pair> pairs;

    /// Microsoft's override bit; Apple's version has none, so it is not written there.
    bool is_override = false;
    bool is_vertical = false;
};

/// A `GPOS` feature: its tag and the lookups it names.
struct babel_test::test_gpos_feature
{
    u32 tag = 0;
    cc::vector<u16> lookups;
};

/// A `GPOS` script, with a default language system naming `features` and no other.
struct babel_test::test_gpos_script
{
    u32 tag = 0;
    cc::vector<u16> features;
};

struct babel_test::test_font
{
    cc::vector<test_glyph> glyphs;
    bool long_loca = true;
    bool with_cmap = true;

    /// A `GPOS` `kern` feature's first lookup: a per-pair subtable of these, then one per-class subtable per entry of
    /// `gpos_classes`.
    /// `GPOS` is written only when some kerning is given.
    cc::vector<test_kern_pair> gpos_pairs;
    cc::vector<test_kern_classes> gpos_classes;

    /// A second lookup of the same feature, per-pair, whose adjustments add to the first's.
    cc::vector<test_kern_pair> gpos_second_lookup;

    /// Writes every lookup as an extension lookup (type 9) wrapping its pair subtables.
    bool gpos_as_extension = false;

    /// Gives each lookup a `kern` feature of its own, the first under script `DFLT` and the second under `latn`.
    /// Without it the script list is empty, and the one `kern` feature names every kerning lookup.
    bool gpos_per_script = false;

    /// A per-pair lookup of these named by a `liga` feature, which kerning must not apply.
    cc::vector<test_kern_pair> gpos_liga_lookup;

    /// Coverage tables in format 2, one range per glyph, rather than format 1.
    bool gpos_coverage_ranges = false;

    /// Class definitions in format 1, a class array over the glyphs' span, rather than format 2.
    bool gpos_class_arrays = false;

    /// Both value records in format 0x0005: an X placement before each X advance, and a second glyph's value too.
    /// Kerning must read neither.
    bool gpos_with_placement = false;

    /// Cuts the written `GPOS` table to this many bytes, when not negative.
    isize gpos_truncate_to = -1;

    /// The legacy `kern` table's subtables, each one's pairs sorted by the writer.
    cc::vector<test_legacy_kern_subtable> legacy_kern;

    /// Writes the legacy table as Apple's version 1 rather than Microsoft's version 0.
    bool legacy_kern_apple = false;
};

namespace babel_test
{

[[nodiscard]] inline u32 test_tag(char const (&s)[5])
{
    return (u32(u8(s[0])) << 24) | (u32(u8(s[1])) << 16) | (u32(u8(s[2])) << 8) | u32(u8(s[3]));
}

/// A coverage table of `glyphs`, sorted: format 1, or format 2 with one range per glyph.
[[nodiscard]] inline cc::vector<byte> encode_coverage(cc::vector<u16> glyphs, bool ranges)
{
    for (auto i = isize(1); i < glyphs.size(); ++i)
        for (auto k = i; k > 0 && glyphs[k - 1] > glyphs[k]; --k)
            cc::swap(glyphs[k - 1], glyphs[k]);
    auto w = be_writer();
    w.u16_(ranges ? 2 : 1);
    w.u16_(u16(glyphs.size()));
    for (auto i = isize(0); i < glyphs.size(); ++i)
    {
        w.u16_(glyphs[i]);
        if (ranges)
        {
            w.u16_(glyphs[i]);
            w.u16_(u16(i));
        }
    }
    return w.bytes;
}

/// The value format of both of a pair's value records.
[[nodiscard]] inline u16 value_format(test_font const& f)
{
    return f.gpos_with_placement ? 0x0005 : 0x0004;
}

/// Both value records of a pair whose first glyph's advance changes by `advance`.
/// The placements and the second glyph's value are nonsense on purpose, so a reader that reads them is caught.
inline void write_value_records(be_writer& w, test_font const& f, i16 advance)
{
    if (f.gpos_with_placement)
        w.i16_(999);
    w.i16_(advance);
    if (f.gpos_with_placement)
    {
        w.i16_(999);
        w.i16_(777);
    }
}

/// A pair-adjustment subtable, format 1: one pair set per left glyph.
[[nodiscard]] inline cc::vector<byte> encode_pair_pairs(cc::vector<test_kern_pair> pairs, test_font const& f)
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
                write_value_records(sets, f, p.value);
            }
    }
    auto const coverage = header + sets.bytes.size();

    auto w = be_writer();
    w.u16_(1);
    w.u16_(u16(coverage));
    w.u16_(value_format(f));
    w.u16_(f.gpos_with_placement ? value_format(f) : u16(0));
    w.u16_(u16(lefts.size()));
    for (auto const o : set_offsets)
        w.u16_(u16(o));
    w.bytes.push_back_range(sets.bytes);
    w.bytes.push_back_range(encode_coverage(lefts, f.gpos_coverage_ranges));
    return w.bytes;
}

/// A class definition putting each of `glyphs` in class 1: format 1, a class array from the lowest glyph to the highest,
/// or format 2, one range per glyph, sorted.
[[nodiscard]] inline cc::vector<byte> encode_class_one(cc::vector<u16> glyphs, bool array)
{
    for (auto i = isize(1); i < glyphs.size(); ++i)
        for (auto k = i; k > 0 && glyphs[k - 1] > glyphs[k]; --k)
            cc::swap(glyphs[k - 1], glyphs[k]);
    auto w = be_writer();
    if (array)
    {
        auto const first = glyphs[0];
        auto const count = isize(glyphs[glyphs.size() - 1] - first + 1);
        w.u16_(1);
        w.u16_(first);
        w.u16_(u16(count));
        auto k = isize(0);
        for (auto g = isize(0); g < count; ++g)
        {
            auto const listed = glyphs[k] == first + g;
            w.u16_(listed ? 1 : 0);
            k += listed ? 1 : 0;
        }
        return w.bytes;
    }
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
[[nodiscard]] inline cc::vector<byte> encode_pair_classes(test_kern_classes const& c, test_font const& f)
{
    auto records = be_writer();
    for (auto const v : {i16(0), i16(0), i16(0), c.value})
        write_value_records(records, f, v);
    auto const class_def1 = isize(16) + records.bytes.size(); // after the header and the four records
    auto const class_def1_bytes = encode_class_one(c.left, f.gpos_class_arrays);
    auto const class_def2 = class_def1 + class_def1_bytes.size();
    auto const class_def2_bytes = encode_class_one(c.right, f.gpos_class_arrays);
    auto const coverage = class_def2 + class_def2_bytes.size();

    auto w = be_writer();
    w.u16_(2);
    w.u16_(u16(coverage));
    w.u16_(value_format(f));
    w.u16_(f.gpos_with_placement ? value_format(f) : u16(0));
    w.u16_(u16(class_def1));
    w.u16_(u16(class_def2));
    w.u16_(2);
    w.u16_(2);
    w.bytes.push_back_range(records.bytes);
    w.bytes.push_back_range(class_def1_bytes);
    w.bytes.push_back_range(class_def2_bytes);
    w.bytes.push_back_range(encode_coverage(c.left, f.gpos_coverage_ranges));
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

/// A `GPOS` table of `lookups`, the `features` naming them, and the `scripts` naming those.
[[nodiscard]] inline cc::vector<byte> encode_gpos(cc::vector<cc::vector<byte>> const& lookups,
                                                  cc::vector<test_gpos_feature> const& features,
                                                  cc::vector<test_gpos_script> const& scripts)
{
    // Each script is its header, then its default language system: no lookup order, no required feature.
    auto script_list = be_writer();
    script_list.u16_(u16(scripts.size()));
    auto at = isize(2 + scripts.size() * 6);
    for (auto const& s : scripts)
    {
        script_list.u32_(s.tag);
        script_list.u16_(u16(at));
        at += 10 + s.features.size() * 2;
    }
    for (auto const& s : scripts)
    {
        script_list.u16_(4);
        script_list.u16_(0);
        script_list.u16_(0);
        script_list.u16_(0xFFFF);
        script_list.u16_(u16(s.features.size()));
        for (auto const i : s.features)
            script_list.u16_(i);
    }

    auto feature_list = be_writer();
    feature_list.u16_(u16(features.size()));
    at = isize(2 + features.size() * 6);
    for (auto const& f : features)
    {
        feature_list.u32_(f.tag);
        feature_list.u16_(u16(at));
        at += 4 + f.lookups.size() * 2;
    }
    for (auto const& f : features)
    {
        feature_list.u16_(0);
        feature_list.u16_(u16(f.lookups.size()));
        for (auto const i : f.lookups)
            feature_list.u16_(i);
    }

    auto lookup_list = be_writer();
    lookup_list.u16_(u16(lookups.size()));
    at = isize(2 + lookups.size() * 2);
    for (auto const& l : lookups)
    {
        lookup_list.u16_(u16(at));
        at += l.size();
    }
    for (auto const& l : lookups)
        lookup_list.bytes.push_back_range(l);

    auto w = be_writer();
    w.u16_(1);
    w.u16_(0);
    w.u16_(10);
    w.u16_(u16(10 + script_list.bytes.size()));
    w.u16_(u16(10 + script_list.bytes.size() + feature_list.bytes.size()));
    w.bytes.push_back_range(script_list.bytes);
    w.bytes.push_back_range(feature_list.bytes);
    w.bytes.push_back_range(lookup_list.bytes);
    return w.bytes;
}

/// The `GPOS` table `f` asks for, uncut; empty when it asks for no kerning.
[[nodiscard]] inline cc::vector<byte> encode_gpos_of(test_font const& f)
{
    if (f.gpos_pairs.empty() && f.gpos_classes.empty() && f.gpos_second_lookup.empty() && f.gpos_liga_lookup.empty())
        return {};

    auto lookups = cc::vector<cc::vector<byte>>();
    auto first = cc::vector<cc::vector<byte>>();
    if (!f.gpos_pairs.empty())
        first.push_back(encode_pair_pairs(f.gpos_pairs, f));
    for (auto const& c : f.gpos_classes)
        first.push_back(encode_pair_classes(c, f));
    lookups.push_back(encode_pair_lookup(first, f.gpos_as_extension));
    if (!f.gpos_second_lookup.empty())
    {
        auto second = cc::vector<cc::vector<byte>>();
        second.push_back(encode_pair_pairs(f.gpos_second_lookup, f));
        lookups.push_back(encode_pair_lookup(second, f.gpos_as_extension));
    }
    auto const kern_lookups = lookups.size();

    auto features = cc::vector<test_gpos_feature>();
    auto scripts = cc::vector<test_gpos_script>();
    if (f.gpos_per_script)
    {
        for (auto i = isize(0); i < kern_lookups; ++i)
            features.push_back({.tag = test_tag("kern"), .lookups = {u16(i)}});
        scripts.push_back({.tag = test_tag("DFLT"), .features = {0}});
        if (kern_lookups > 1)
            scripts.push_back({.tag = test_tag("latn"), .features = {1}});
    }
    else
    {
        auto all = cc::vector<u16>();
        for (auto i = isize(0); i < kern_lookups; ++i)
            all.push_back(u16(i));
        features.push_back({.tag = test_tag("kern"), .lookups = all});
    }

    if (!f.gpos_liga_lookup.empty())
    {
        auto liga = cc::vector<cc::vector<byte>>();
        liga.push_back(encode_pair_pairs(f.gpos_liga_lookup, f));
        lookups.push_back(encode_pair_lookup(liga, f.gpos_as_extension));
        features.push_back({.tag = test_tag("liga"), .lookups = {u16(lookups.size() - 1)}});
        for (auto& s : scripts)
            s.features.push_back(u16(features.size() - 1));
    }
    return encode_gpos(lookups, features, scripts);
}

/// A legacy `kern` table of `subtables`, all format 0: Microsoft's version 0, or Apple's version 1.
[[nodiscard]] inline cc::vector<byte> encode_legacy_kern(cc::vector<test_legacy_kern_subtable> const& subtables,
                                                         bool apple)
{
    auto const key = [](test_kern_pair const& p) { return (u32(p.left) << 16) | p.right; };
    auto w = be_writer();
    if (apple)
    {
        w.u32_(0x00010000);
        w.u32_(u32(subtables.size()));
    }
    else
    {
        w.u16_(0);
        w.u16_(u16(subtables.size()));
    }
    for (auto const& st : subtables)
    {
        auto pairs = st.pairs;
        for (auto i = isize(1); i < pairs.size(); ++i)
            for (auto k = i; k > 0 && key(pairs[k - 1]) > key(pairs[k]); --k)
                cc::swap(pairs[k - 1], pairs[k]);
        auto const length = (apple ? 16 : 14) + pairs.size() * 6;
        if (apple)
        {
            w.u32_(u32(length));
            w.u8_(st.is_vertical ? 0x80 : 0x00);
            w.u8_(0); // format 0
            w.u16_(0);
        }
        else
        {
            w.u16_(0);
            w.u16_(u16(length));
            w.u16_(u16((st.is_vertical ? 0x0000 : 0x0001) | (st.is_override ? 0x0008 : 0x0000)));
        }
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
    gpos.bytes = encode_gpos_of(f);
    if (f.gpos_truncate_to >= 0 && f.gpos_truncate_to < gpos.bytes.size())
        gpos.bytes.resize_to_defaulted(f.gpos_truncate_to);
    if (!gpos.bytes.empty())
        tables.push_back({"GPOS", &gpos});
    auto kern = be_writer();
    if (!f.legacy_kern.empty())
    {
        kern.bytes = encode_legacy_kern(f.legacy_kern, f.legacy_kern_apple);
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
