#include <babel-serializer/font/font.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The fonts here are built byte by byte, so every encoding the reader decodes is one this file chose on purpose:
// short and long coordinate deltas, repeated flags, a composite with a 2x2 transform, and both loca widths.

namespace
{
struct be_writer
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

struct test_glyph
{
    cc::vector<babel::font::glyf_point> points;
    cc::vector<i32> contour_ends;
    cc::vector<babel::font::glyf_component> components;
};

/// One simple glyph record, encoded the way a font tool would: the shortest delta each coordinate allows, and runs of
/// equal flags folded with the repeat bit.
[[nodiscard]] cc::vector<byte> encode_simple(test_glyph const& g)
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

[[nodiscard]] cc::vector<byte> encode_composite(test_glyph const& g)
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

struct test_font
{
    cc::vector<test_glyph> glyphs;
    bool long_loca = true;
    bool with_cmap = true;
};

/// A one-face font of `head`, `maxp`, `hhea`, `hmtx`, `cmap`, `loca` and `glyf`.
/// Characters 'A', 'B', ... map to glyphs 1, 2, ...; glyph 0 is an empty `.notdef`.
/// Two long metrics, so every glyph from 2 on shares glyph 1's advance.
[[nodiscard]] cc::vector<byte> build_font(test_font const& f)
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
        char const* tag;
        be_writer* data;
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

[[nodiscard]] test_glyph letter()
{
    auto g = test_glyph();
    // short deltas, a long one (+400), a repeated coordinate, and two off-curve points in a row
    g.points.push_back({.position = tg::pos2i(10, 0), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(410, 0), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(410, 300), .on_curve = false});
    g.points.push_back({.position = tg::pos2i(200, 300), .on_curve = false});
    g.points.push_back({.position = tg::pos2i(10, 250), .on_curve = true});
    g.contour_ends.push_back(4);
    // a second contour, a triangle with one negative short delta
    g.points.push_back({.position = tg::pos2i(100, 100), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(200, 100), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(150, 50), .on_curve = true});
    g.contour_ends.push_back(7);
    return g;
}

[[nodiscard]] test_font letters_font()
{
    auto f = test_font();
    f.glyphs.push_back({}); // .notdef, empty
    f.glyphs.push_back(letter());

    auto composite = test_glyph();
    composite.components.push_back(
        {.glyph = babel::font::glyph_id(1), .flags = 0x0001 | 0x0002 | 0x0020, .arg1 = 300, .arg2 = -20});
    composite.components.push_back({.glyph = babel::font::glyph_id(1),
                                    .flags = 0x0002 | 0x0080,
                                    .arg1 = 5,
                                    .arg2 = -7,
                                    .xx = 0.5f,
                                    .xy = 0.25f,
                                    .yx = -0.25f,
                                    .yy = 1.0f});
    f.glyphs.push_back(composite);
    f.glyphs.push_back({}); // a space: no record at all
    return f;
}
} // namespace

TEST("babel::font - the face-wide numbers are read as stored")
{
    auto const bytes = build_font(letters_font());
    auto const r = babel::font::read(cc::span<byte const>(bytes));
    REQUIRE(r.has_value());
    auto const& m = r.value().metrics();
    CHECK(m.units_per_em == 1000);
    CHECK(m.glyph_count == 4);
    CHECK(m.ascender == 800);
    CHECK(m.descender == -200);
    CHECK(m.line_gap == 90);
    CHECK(m.bounds.min == tg::pos2i(-50, -200));
    CHECK(m.bounds.max == tg::pos2i(900, 800));
    CHECK(r.value().outlines() == babel::font::outline_format::truetype);
}

TEST("babel::font - cmap maps a character to its glyph, and a missing one to nothing")
{
    auto const f = babel::font::read(cc::span<byte const>(build_font(letters_font()))).value();
    CHECK(f.glyph_for(U'A') == babel::font::glyph_id(1));
    CHECK(f.glyph_for(U'B') == babel::font::glyph_id(2));
    CHECK(f.glyph_for(U'C') == babel::font::glyph_id(3));
    CHECK(!f.glyph_for(U'Z').has_value());
    CHECK(!f.glyph_for(U'@').has_value());
    CHECK(!f.glyph_for(char32_t(0x1F600)).has_value());
}

TEST("babel::font - a glyph past the long metrics shares the last advance and keeps its own bearing")
{
    auto const f = babel::font::read(cc::span<byte const>(build_font(letters_font()))).value();
    CHECK(f.horizontal(babel::font::glyph_id(0)).advance == 250);
    CHECK(f.horizontal(babel::font::glyph_id(1)).advance == 600);
    CHECK(f.horizontal(babel::font::glyph_id(1)).left_side_bearing == 10);
    CHECK(f.horizontal(babel::font::glyph_id(3)).advance == 600);
    CHECK(f.horizontal(babel::font::glyph_id(3)).left_side_bearing == 23);
}

TEST("babel::font - a simple outline is its points and contour ends, exactly as stored")
{
    for (auto const long_loca : {true, false})
    {
        auto font = letters_font();
        font.long_loca = long_loca;
        auto const f = babel::font::read(cc::span<byte const>(build_font(font))).value();
        auto const o = f.outline(babel::font::glyph_id(1));
        REQUIRE(o.has_value());
        auto const expected = letter();
        REQUIRE(o.value().points.size() == expected.points.size());
        for (auto i = isize(0); i < expected.points.size(); ++i)
        {
            CHECK(o.value().points[i].position == expected.points[i].position).dump("point", i);
            CHECK(o.value().points[i].on_curve == expected.points[i].on_curve).dump("point", i);
        }
        REQUIRE(o.value().contour_ends.size() == 2);
        CHECK(o.value().contour_ends[0] == 4);
        CHECK(o.value().contour_ends[1] == 7);
        CHECK(!o.value().is_composite());
        CHECK(o.value().bounds.max == tg::pos2i(500, 700));
    }
}

TEST("babel::font - a composite outline lists its components, and no points")
{
    auto const f = babel::font::read(cc::span<byte const>(build_font(letters_font()))).value();
    auto const o = f.outline(babel::font::glyph_id(2));
    REQUIRE(o.has_value());
    REQUIRE(o.value().is_composite());
    CHECK(o.value().points.empty());
    REQUIRE(o.value().components.size() == 2);

    auto const& a = o.value().components[0];
    CHECK(a.glyph == babel::font::glyph_id(1));
    CHECK(a.args_are_offset);
    CHECK(a.arg1 == 300);
    CHECK(a.arg2 == -20);
    CHECK(a.xx == 1.0f);

    auto const& b = o.value().components[1];
    CHECK(b.arg1 == 5);
    CHECK(b.arg2 == -7);
    CHECK(b.xx == 0.5f);
    CHECK(b.xy == 0.25f);
    CHECK(b.yx == -0.25f);
    CHECK(b.yy == 1.0f);
}

TEST("babel::font - a glyph with no record is empty, not an error")
{
    auto const f = babel::font::read(cc::span<byte const>(build_font(letters_font()))).value();
    auto const space = f.outline(babel::font::glyph_id(3));
    REQUIRE(space.has_value());
    CHECK(space.value().is_empty());
    CHECK(f.outline(babel::font::glyph_id(0)).value().is_empty());
}

TEST("babel::font - a file cut short or missing a table is an error, never a crash")
{
    auto const whole = build_font(letters_font());
    // every prefix: the reader may refuse it or read it, and must do neither out of bounds
    for (auto n = isize(0); n < whole.size(); n += 7)
    {
        auto const r = babel::font::read(cc::span<byte const>(whole).first_n(n));
        if (r.has_value())
            for (auto g = 0; g < r.value().glyph_count(); ++g)
                (void)r.value().outline(babel::font::glyph_id(g));
    }

    auto no_cmap = letters_font();
    no_cmap.with_cmap = false;
    auto const r = babel::font::read(cc::span<byte const>(build_font(no_cmap)));
    REQUIRE(r.has_error());
    CHECK(r.error().to_string().contains("cmap"));

    auto const garbage = cc::vector<byte>::create_filled(64, byte(0x5A));
    CHECK(babel::font::read(cc::span<byte const>(garbage)).has_error());
}

TEST("babel::font - a face index past a single font is an error")
{
    auto const bytes = build_font(letters_font());
    CHECK(babel::font::read(cc::span<byte const>(bytes), 1).has_error());
}
