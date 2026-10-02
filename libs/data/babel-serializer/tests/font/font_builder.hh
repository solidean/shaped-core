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
struct test_font;  // the glyphs of a one-face font, and two layout switches
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

struct babel_test::test_font
{
    cc::vector<test_glyph> glyphs;
    bool long_loca = true;
    bool with_cmap = true;
};

namespace babel_test
{

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
