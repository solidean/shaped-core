#include "font_builder.hh"

#include <babel-serializer/font/font.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The fonts are built byte by byte by font_builder.hh; this file holds the glyphs they are built from.

namespace
{
using babel_test::build_font;
using babel_test::test_font;
using babel_test::test_glyph;

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

TEST("babel::font - a face with no glyphs is refused, since every face has a .notdef")
{
    // Read, it would send every lookup to glyph 0, which the face's queries require to exist.
    auto const r = babel::font::read(cc::span<byte const>(build_font(test_font())));
    REQUIRE(r.has_error());
    CHECK(r.error().to_string().contains("no glyphs"));
}
