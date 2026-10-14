#include <babel-serializer/image/image.hh>
#include <babel-serializer/image/tga.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
namespace tga = babel::tga;

/// The header fields a test varies; the defaults are an uncompressed 24-bit image stored bottom-up.
struct tga_spec
{
    int type = 2;
    int width = 2;
    int height = 2;
    int depth = 24;
    int descriptor = 0;
    int color_map_type = 0;
    int color_map_first = 0;
    int color_map_length = 0;
    int color_map_depth = 0;
    cc::string_view id = {};
};

/// A TGA file: the header, the ID, then `body` (the palette, if any, and the pixel data) as it is.
cc::vector<byte> make_tga(tga_spec const& spec, cc::span<u8 const> body, bool footer = false)
{
    auto out = cc::vector<byte>::create_filled(18, byte(0));
    out[0] = byte(u8(spec.id.size()));
    out[1] = byte(u8(spec.color_map_type));
    out[2] = byte(u8(spec.type));
    cc::store_bytes_le<u16>(out, 3, u16(spec.color_map_first));
    cc::store_bytes_le<u16>(out, 5, u16(spec.color_map_length));
    out[7] = byte(u8(spec.color_map_depth));
    cc::store_bytes_le<u16>(out, 12, u16(spec.width));
    cc::store_bytes_le<u16>(out, 14, u16(spec.height));
    out[16] = byte(u8(spec.depth));
    out[17] = byte(u8(spec.descriptor));
    for (auto const c : spec.id)
        out.push_back(byte(c));
    for (auto const b : body)
        out.push_back(byte(b));
    if (footer)
    {
        for (auto i = 0; i < 8; ++i)
            out.push_back(byte(0)); // extension and developer area offsets
        for (auto const c : cc::string_view("TRUEVISION-XFILE.\0", 18))
            out.push_back(byte(c));
    }
    return out;
}

bool pixel_is(tga::data const& d, int x, int y, cc::span<int const> expected)
{
    auto const at = (isize(y) * d.width + x) * d.channels;
    for (auto c = 0; c < d.channels; ++c)
        if (int(u8(d.pixels[at + c])) != expected[c])
            return false;
    return true;
}
} // namespace

TEST("tga - uncompressed 24-bit, stored bottom-up, comes back top-left and rgb")
{
    // The bottom row is stored first, each pixel as b, g, r.
    u8 const body[] = {
        1,  2,  3,  4,  5,  6,  // bottom row: (r3 g2 b1), (r6 g5 b4)
        11, 12, 13, 14, 15, 16, // top row
    };
    auto const decoded = tga::read(make_tga({}, body));
    REQUIRE(decoded.has_value());
    auto const& d = decoded.value();
    CHECK(d.width == 2);
    CHECK(d.height == 2);
    CHECK(d.channels == 3);
    CHECK(d.type == tga::image_type::true_color);
    CHECK(!d.run_length_encoded);
    CHECK(d.stored_bottom_up);

    int const top_left[] = {13, 12, 11};
    int const bottom_right[] = {6, 5, 4};
    CHECK(pixel_is(d, 0, 0, top_left));
    CHECK(pixel_is(d, 1, 1, bottom_right));
}

TEST("tga - the descriptor's origin bits are honored")
{
    u8 const body[] = {1, 2, 3, 4, 5, 6, 11, 12, 13, 14, 15, 16};

    SECTION("top-down")
    {
        auto const decoded = tga::read(make_tga({.descriptor = 0x20}, body));
        REQUIRE(decoded.has_value());
        CHECK(!decoded.value().stored_bottom_up);
        int const first[] = {3, 2, 1};
        CHECK(pixel_is(decoded.value(), 0, 0, first));
    }

    SECTION("right-to-left and top-down")
    {
        auto const decoded = tga::read(make_tga({.descriptor = 0x30}, body));
        REQUIRE(decoded.has_value());
        CHECK(decoded.value().stored_right_to_left);
        int const first[] = {3, 2, 1};
        CHECK(pixel_is(decoded.value(), 1, 0, first));
    }
}

TEST("tga - 32-bit keeps alpha, and the ID field is read")
{
    u8 const body[] = {10, 20, 30, 40};
    auto const decoded
        = tga::read(make_tga({.width = 1, .height = 1, .depth = 32, .descriptor = 0x28, .id = "hello"}, body));
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().channels == 4);
    CHECK(decoded.value().alpha_bits == 8);
    CHECK(decoded.value().image_id == "hello");
    int const expected[] = {30, 20, 10, 40};
    CHECK(pixel_is(decoded.value(), 0, 0, expected));
}

TEST("tga - 15/16-bit pixels widen each 5-bit channel")
{
    // 0x7C00 is full red; with the top bit set and an attribute bit declared, it carries alpha.
    u8 const body[] = {0x00, 0xFC};
    auto const with_alpha = tga::read(make_tga({.width = 1, .height = 1, .depth = 16, .descriptor = 1}, body));
    REQUIRE(with_alpha.has_value());
    CHECK(with_alpha.value().channels == 4);
    int const red_opaque[] = {255, 0, 0, 255};
    CHECK(pixel_is(with_alpha.value(), 0, 0, red_opaque));

    auto const without = tga::read(make_tga({.width = 1, .height = 1, .depth = 15}, body));
    REQUIRE(without.has_value());
    CHECK(without.value().channels == 3);
}

TEST("tga - the descriptor decides alpha at every depth, and an all-zero alpha reads opaque")
{
    SECTION("a 32-bit pixel with no attribute bits declared is rgb, its fourth byte padding")
    {
        u8 const body[] = {10, 20, 30, 0};
        auto const decoded = tga::read(make_tga({.width = 1, .height = 1, .depth = 32}, body));
        REQUIRE(decoded.has_value());
        CHECK(decoded.value().channels == 3);
        int const expected[] = {30, 20, 10};
        CHECK(pixel_is(decoded.value(), 0, 0, expected));
    }

    SECTION("32-bit alpha declared and zero everywhere reads opaque, and says so")
    {
        u8 const body[] = {10, 20, 30, 0, 40, 50, 60, 0};
        auto const decoded = tga::read(make_tga({.width = 2, .height = 1, .depth = 32, .descriptor = 8}, body));
        REQUIRE(decoded.has_value());
        CHECK(decoded.value().channels == 4);
        CHECK(decoded.value().alpha_was_all_zero);
        int const expected[] = {30, 20, 10, 255};
        CHECK(pixel_is(decoded.value(), 0, 0, expected));
    }

    SECTION("16-bit with the attribute bit declared and bit 15 clear everywhere reads opaque")
    {
        u8 const body[] = {0x00, 0x7C}; // full red, bit 15 clear
        auto const decoded = tga::read(make_tga({.width = 1, .height = 1, .depth = 16, .descriptor = 1}, body));
        REQUIRE(decoded.has_value());
        CHECK(decoded.value().alpha_was_all_zero);
        int const expected[] = {255, 0, 0, 255};
        CHECK(pixel_is(decoded.value(), 0, 0, expected));
    }

    SECTION("an alpha that is zero at only some pixels is kept as it is")
    {
        u8 const body[] = {10, 20, 30, 0, 40, 50, 60, 128};
        auto const decoded = tga::read(make_tga({.width = 2, .height = 1, .depth = 32, .descriptor = 8}, body));
        REQUIRE(decoded.has_value());
        CHECK(!decoded.value().alpha_was_all_zero);
        int const first[] = {30, 20, 10, 0};
        CHECK(pixel_is(decoded.value(), 0, 0, first));
    }
}

TEST("tga - run-length packets may cross rows")
{
    // 3x2, 32-bit, top-down: a run of 4 red pixels spans row 0 into row 1, then 2 raw pixels.
    u8 const body[] = {
        0x83, 0,   0, 255, 255,                // run of 4: b g r a
        0x01, 255, 0, 0,   128, 0, 255, 0, 64, // raw 2: blue, green
    };
    auto const decoded = tga::read(make_tga({.type = 10, .width = 3, .depth = 32, .descriptor = 0x28}, body));
    REQUIRE(decoded.has_value());
    auto const& d = decoded.value();
    CHECK(d.run_length_encoded);
    int const red[] = {255, 0, 0, 255};
    int const blue[] = {0, 0, 255, 128};
    int const green[] = {0, 255, 0, 64};
    CHECK(pixel_is(d, 2, 0, red));
    CHECK(pixel_is(d, 0, 1, red));
    CHECK(pixel_is(d, 1, 1, blue));
    CHECK(pixel_is(d, 2, 1, green));
}

TEST("tga - grayscale, raw and run-length")
{
    u8 const raw[] = {10, 20, 30, 40};
    auto const gray = tga::read(make_tga({.type = 3, .depth = 8, .descriptor = 0x20}, raw));
    REQUIRE(gray.has_value());
    CHECK(gray.value().channels == 1);
    CHECK(gray.value().type == tga::image_type::grayscale);
    CHECK(u8(gray.value().pixels[3]) == 40);

    u8 const rle[] = {0x83, 77};
    auto const runs = tga::read(make_tga({.type = 11, .depth = 8}, rle));
    REQUIRE(runs.has_value());
    for (auto i = 0; i < 4; ++i)
        CHECK(u8(runs.value().pixels[i]) == 77);

    u8 const with_alpha[] = {50, 200};
    auto const ga = tga::read(make_tga({.type = 3, .width = 1, .height = 1, .depth = 16, .descriptor = 8}, with_alpha));
    REQUIRE(ga.has_value());
    CHECK(ga.value().channels == 2);
    CHECK(u8(ga.value().pixels[1]) == 200);
}

TEST("tga - color-mapped images look indices up from the map's first entry")
{
    // A 24-bit palette whose first entry is index 5: entries 5 (red) and 6 (green).
    u8 const body[] = {
        0, 0, 255, 0, 255, 0, // palette, b g r
        5, 6, 6,   5,         // indices
    };
    auto const spec = tga_spec{.type = 1,
                               .depth = 8,
                               .descriptor = 0x20,
                               .color_map_type = 1,
                               .color_map_first = 5,
                               .color_map_length = 2,
                               .color_map_depth = 24};
    auto const decoded = tga::read(make_tga(spec, body));
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().type == tga::image_type::color_mapped);
    CHECK(decoded.value().channels == 3);
    int const red[] = {255, 0, 0};
    int const green[] = {0, 255, 0};
    CHECK(pixel_is(decoded.value(), 0, 0, red));
    CHECK(pixel_is(decoded.value(), 1, 0, green));

    u8 const outside[] = {0, 0, 255, 0, 255, 0, 5, 6, 7, 5};
    CHECK(tga::read(make_tga(spec, outside)).has_error());
}

TEST("tga - a short or inconsistent file is an error")
{
    u8 const body[] = {1, 2, 3, 4, 5, 6, 11, 12, 13, 14, 15, 16};
    auto const file = make_tga({}, body);
    for (auto size = isize(0); size < file.size(); ++size)
        CHECK(tga::read(cc::span<byte const>(file).first_n(size)).has_error());

    u8 const overrun[] = {0x84, 1, 2, 3}; // a run of 5 into 4 pixels
    CHECK(tga::read(make_tga({.type = 10}, overrun)).has_error());

    u8 const cut[] = {0x81, 1, 2, 3}; // a run of 2, then nothing
    CHECK(tga::read(make_tga({.type = 10}, cut)).has_error());

    CHECK(tga::read(make_tga({.type = 0}, body)).has_error());
    CHECK(tga::read(make_tga({.type = 4}, body)).has_error());
    CHECK(tga::read(make_tga({.depth = 12}, body)).has_error());
    CHECK(tga::read(make_tga({.width = 0}, body)).has_error());
    CHECK(tga::read(make_tga({.type = 1, .depth = 8}, body)).has_error()); // color-mapped, no map
}

TEST("tga - the aggregator detects TGA by footer and by header")
{
    u8 const body[] = {1, 2, 3, 4, 5, 6, 11, 12, 13, 14, 15, 16};

    auto const with_footer = make_tga({}, body, true);
    CHECK(tga::has_footer(with_footer));
    auto const fmt = babel::image::detect_format(with_footer);
    REQUIRE(fmt.has_value());
    CHECK(fmt.value() == babel::image::format::tga);

    auto const plain = make_tga({}, body);
    CHECK(!tga::has_footer(plain));
    CHECK(tga::has_plausible_header(plain));
    auto const guessed = babel::image::detect_format(plain);
    REQUIRE(guessed.has_value());
    CHECK(guessed.value() == babel::image::format::tga);

    auto const img = babel::image::read(with_footer);
    REQUIRE(img.has_value());
    CHECK(img.value().channels == 3);
    CHECK(img.value().comp == babel::image::component::u8);
    CHECK(u8(img.value().pixels[0]) == 13);

    // Explicit, the format skips detection.
    auto const explicit_read = babel::image::read(plain, babel::image::format::tga);
    REQUIRE(explicit_read.has_value());

    // Text is not taken for a TGA.
    auto const text = cc::string_view("hello world, this is not an image at all");
    CHECK(babel::image::detect_format(text.as_bytes()).has_error());

    CHECK(babel::image::encode(img.value(), babel::image::format::tga).has_error());
}

TEST("tga - a decode larger than the budget is refused before allocating")
{
    u8 const body[] = {1, 2, 3, 4, 5, 6, 11, 12, 13, 14, 15, 16};
    CHECK(tga::read(make_tga({}, body), {.max_decoded_bytes = 12}).has_value());
    CHECK(tga::read(make_tga({}, body), {.max_decoded_bytes = 11}).has_error());

    // Valid run-length data: 64 x 64 rgba in 32 runs of 128 pixels, 16 KiB decoded from 160 bytes.
    // Only the budget tells the two reads apart.
    auto packets = cc::vector<u8>();
    for (auto i = 0; i < 32; ++i)
        packets.push_back_range(cc::span<u8 const>(cc::fixed_array<u8, 5>{0xFF, 1, 2, 3, 4}));
    auto const rle = make_tga({.type = 10, .width = 64, .height = 64, .depth = 32, .descriptor = 8}, packets);
    CHECK(tga::read(rle, {.max_decoded_bytes = 64 * 64 * 4}).has_value());
    CHECK(tga::read(rle, {.max_decoded_bytes = 64 * 64 * 4 - 1}).has_error());
}
