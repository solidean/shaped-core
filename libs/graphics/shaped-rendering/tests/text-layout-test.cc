#include <font/font_builder.hh>
#include <nexus/test.hh>
#include <shaped-rendering/text_layout.hh>

using namespace cc::primitive_defines;

// Text layout on a crafted face: 1000 units per em, ascender 800, descender -200, line gap 90.
// 'A', 'B', 'C' are glyphs 1 to 3, each 600 units wide; a space has no glyph and sets as .notdef, 250 wide.

namespace
{
[[nodiscard]] babel_test::test_glyph square()
{
    auto g = babel_test::test_glyph();
    g.points.push_back({.position = tg::pos2i(0, 0), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(500, 0), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(500, 700), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(0, 700), .on_curve = true});
    g.contour_ends.push_back(3);
    return g;
}

[[nodiscard]] babel::font::face face_of(babel_test::test_font f)
{
    f.glyphs.push_back({});
    for (auto i = 0; i < 3; ++i)
        f.glyphs.push_back(square());
    return babel::font::read(babel_test::build_font(f)).value();
}

[[nodiscard]] babel::font::face plain()
{
    return face_of({});
}

[[nodiscard]] tg::pos2f origin_of(sr::text_layout const& l, isize i)
{
    return l.glyphs[i].origin;
}
} // namespace

TEST("sr::layout_text - glyphs advance along the first baseline, one ascender below the top")
{
    auto const l = sr::layout_text(plain(), "AB", {.size = 1000});
    REQUIRE(l.glyphs.size() == 2);
    CHECK(origin_of(l, 0) == tg::pos2f(0, 800));
    CHECK(origin_of(l, 1) == tg::pos2f(600, 800));
    CHECK(l.line_count == 1);
    CHECK(l.box.max == tg::pos2f(1200, 1000)); // ascender down to the descender
}

TEST("sr::layout_text - size scales every distance")
{
    auto const l = sr::layout_text(plain(), "AB", {.size = 10});
    CHECK(l.scale == 0.01f);
    CHECK(origin_of(l, 1) == tg::pos2f(6, 8));
}

TEST("sr::layout_text - a kerned pair moves the second glyph by the face's adjustment")
{
    auto f = babel_test::test_font();
    f.gpos_pairs = {{.left = 1, .right = 2, .value = -100}};
    auto const l = sr::layout_text(face_of(f), "ABA", {.size = 1000});
    CHECK(origin_of(l, 1)[0] == 500.0f);
    CHECK(origin_of(l, 2)[0] == 1100.0f); // (B, A) is not kerned
    CHECK(l.box.max[0] == 1700.0f);
}

TEST("sr::layout_text - a line break starts the next baseline one line spacing down")
{
    auto const l = sr::layout_text(plain(), "A\nB", {.size = 1000, .line_height = 1.5f});
    REQUIRE(l.glyphs.size() == 2);
    CHECK(l.line_count == 2);
    CHECK(origin_of(l, 1) == tg::pos2f(0, 800 + 1.5f * 1090));
    CHECK(l.box.max[1] == 800 + 1.5f * 1090 + 200);

    // a carriage return before the break is the break's
    CHECK(sr::layout_text(plain(), "A\r\nB", {.size = 1000}).glyphs.size() == 2);
}

TEST("sr::layout_text - lines align within the widest one")
{
    auto const centred = sr::layout_text(plain(), "AB\nA", {.size = 1000, .align = sr::text_align::center});
    CHECK(origin_of(centred, 2)[0] == 300.0f);
    auto const right = sr::layout_text(plain(), "AB\nA", {.size = 1000, .align = sr::text_align::right});
    CHECK(origin_of(right, 2)[0] == 600.0f);
    CHECK(origin_of(right, 0)[0] == 0.0f);
}

TEST("sr::layout_text - a line wraps at its last space, and the next word starts the next line")
{
    // "AB " is 1450 wide; the second A would end at 2050, past 1500, so it wraps after the space
    auto const l = sr::layout_text(plain(), "AB AB", {.size = 1000, .max_width = 1500});
    REQUIRE(l.glyphs.size() == 5);
    CHECK(l.line_count == 2);
    CHECK(origin_of(l, 3) == tg::pos2f(0, 800 + 1090));
    CHECK(origin_of(l, 4) == tg::pos2f(600, 800 + 1090));
    CHECK(l.box.max[0] == 1500.0f); // a wrapping style's box is as wide as it allows

    // a word wider than the limit on its own is not split
    auto const long_word = sr::layout_text(plain(), "ABC", {.size = 1000, .max_width = 1000});
    CHECK(long_word.line_count == 1);
}
