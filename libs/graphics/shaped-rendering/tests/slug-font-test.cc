#include <clean-core/container/vector.hh>
#include <font/font_builder.hh>
#include <nexus/test.hh>
#include <shaped-rendering/slug_font.hh>
#include <shaped-rendering/slug_routine.hh>

using namespace cc::primitive_defines;

// slug_font's one-line layout, on a face built byte by byte: no device, the atlas only places.
//
// The builder's face maps 'A', 'B', 'C' to glyphs 1, 2, 3, with 1000 units per em.
// `.notdef` advances 250 and every other glyph 600, so an origin tells which glyph advanced the pen.

namespace
{
[[nodiscard]] babel_test::test_glyph square(i32 size)
{
    auto g = babel_test::test_glyph();
    g.points.push_back({.position = tg::pos2i(0, 0), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(size, 0), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(size, size), .on_curve = true});
    g.points.push_back({.position = tg::pos2i(0, size), .on_curve = true});
    g.contour_ends.push_back(3);
    return g;
}

/// `.notdef` a square, 'A' a smaller one, 'B' empty like a space, and 'C' a composite that cannot resolve.
[[nodiscard]] sr::slug_font test_font()
{
    auto font = babel_test::test_font();
    font.glyphs.push_back(square(200));
    font.glyphs.push_back(square(500));
    font.glyphs.push_back({});

    // The second component attaches by point matching to point 50 of the first, which has four.
    auto broken = babel_test::test_glyph();
    broken.components.push_back({.glyph = babel::font::glyph_id(1), .flags = u16(0x0002 | 0x0020)});
    broken.components.push_back({.glyph = babel::font::glyph_id(1), .flags = u16(0), .arg1 = 50, .arg2 = 0});
    font.glyphs.push_back(broken);

    auto const bytes = babel_test::build_font(font);
    return sr::slug_font(babel::font::read(cc::span<byte const>(bytes)).value());
}

/// The origin an instance of glyph `g` gets with its pen at x, the way append_line places it at size 1000.
[[nodiscard]] tg::vec2f origin_of(sr::slug_font& font, u16 g, f32 x)
{
    auto const ref = font.glyph(babel::font::glyph_id(g)).value();
    return sr::make_slug_instance(ref, tg::pos2f(x, 0), tg::vec2f(1, 0), tg::vec2f(0, 1), tg::vec4f(1, 1, 1, 1)).origin;
}

void draw(sr::slug_font& font, cc::vector<sr::slug_instance>& out, cc::string_view text)
{
    font.append_line(out, text, tg::pos2f(0, 0), 1000.0f, tg::vec4f(1, 1, 1, 1));
}
} // namespace

TEST("sr::slug_font - each glyph advances by its own advance width")
{
    auto font = test_font();
    auto instances = cc::vector<sr::slug_instance>();
    draw(font, instances, "AAA");
    REQUIRE(instances.size() == 3);
    CHECK(instances[0].origin == origin_of(font, 1, 0));
    CHECK(instances[1].origin == origin_of(font, 1, 600));
    CHECK(instances[2].origin == origin_of(font, 1, 1200));
    CHECK(font.line_width("AAA", 1000.0f) == 1800.0f);
}

TEST("sr::slug_font - a character the face does not map draws .notdef")
{
    auto font = test_font();
    auto instances = cc::vector<sr::slug_instance>();
    draw(font, instances, "ZA");
    REQUIRE(instances.size() == 2);
    CHECK(instances[0].origin == origin_of(font, 0, 0));
    CHECK(instances[1].origin == origin_of(font, 1, 250));
}

TEST("sr::slug_font - an empty glyph emits no instance and still advances")
{
    auto font = test_font();
    auto instances = cc::vector<sr::slug_instance>();
    draw(font, instances, "ABA");
    REQUIRE(instances.size() == 2);
    CHECK(instances[1].origin == origin_of(font, 1, 1200));
    CHECK(font.line_width("ABA", 1000.0f) == 1800.0f);
}

TEST("sr::slug_font - a malformed sequence draws U+FFFD and loses none of the bytes after it")
{
    // 0xC3 announces a second byte, and 'A' is not one: that is U+FFFD, unmapped so .notdef, then the 'A' itself.
    auto font = test_font();
    auto instances = cc::vector<sr::slug_instance>();
    draw(font, instances,
         "\xC3"
         "A");
    REQUIRE(instances.size() == 2);
    CHECK(instances[0].origin == origin_of(font, 0, 0));
    CHECK(instances[1].origin == origin_of(font, 1, 250));
}

TEST("sr::slug_font - a glyph that fails to compile warns once, draws nothing, and still advances")
{
    auto font = test_font();
    CHECK(font.glyph(babel::font::glyph_id(3)).has_error());

    auto font_drawn = test_font();
    nx::expect_warning("did not compile", nx::exactly(1));
    auto instances = cc::vector<sr::slug_instance>();
    draw(font_drawn, instances, "CA");
    draw(font_drawn, instances, "CA");
    REQUIRE(instances.size() == 2);
    CHECK(instances[0].origin == origin_of(font_drawn, 1, 600));
    CHECK(instances[1].origin == origin_of(font_drawn, 1, 600));
}
