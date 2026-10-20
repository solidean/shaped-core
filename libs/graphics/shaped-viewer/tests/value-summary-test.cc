#include <babel-serializer/image/bcn.hh>
#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <nexus/test.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-viewer/resources/resource_data.hh>
#include <shaped-viewer/resources/value_summary.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// CPU-only tests of sv::value_summary: what a texture's texels or a mesh attribute's elements hold, for an emitter's selection weight.
// The rule that reads them is emitter-test.cc's.

namespace
{
/// Whether every lane of `v` is finite.
[[nodiscard]] bool all_finite(tg::vec4f v)
{
    return tg::is_finite(v[0]) && tg::is_finite(v[1]) && tg::is_finite(v[2]) && tg::is_finite(v[3]);
}
} // namespace

TEST("sv - a texture's summary is what the sampler reads, and exact only where every texel was read")
{
    SECTION("an sRGB texture averages in linear, and a missing alpha reads 1")
    {
        auto const texels = cc::array<u8>{255, 0, 0, 255, 0, 0, 0, 255, 188, 188, 188, 255, 0, 0, 0, 0};
        auto const s
            = sv::summarize_texels(cc::span<u8 const>(texels).as_bytes(), sg::pixel_format::rgba8_unorm_srgb, 2, 2);
        REQUIRE(s.has_value());
        CHECK(s.value().exact);
        // sRGB 188 is linear 0.5 to within a hundredth.
        CHECK(tg::abs(s.value().mean[0] - (1.0f + 0.5f) / 4) < 0.005f);
        CHECK(tg::abs(s.value().mean[1] - 0.5f / 4) < 0.005f);
        CHECK(s.value().max[0] == 1.0f);
        CHECK(s.value().min[1] == 0.0f);
        CHECK(tg::abs(s.value().mean[3] - 0.75f) < 1e-6f);

        auto const red = cc::array<u8>{51, 102};
        auto const r = sv::summarize_texels(cc::span<u8 const>(red).as_bytes(), sg::pixel_format::r8_unorm, 2, 1);
        REQUIRE(r.has_value());
        CHECK(tg::abs(r.value().mean[0] - 0.3f) < 1e-6f);
        CHECK(r.value().mean[1] == 0.0f);
        CHECK(r.value().mean[3] == 1.0f);
    }

    SECTION("bgra reads blue from its first byte")
    {
        auto const texel = cc::array<u8>{255, 0, 0, 255};
        auto const s = sv::summarize_texels(cc::span<u8 const>(texel).as_bytes(), sg::pixel_format::bgra8_unorm, 1, 1);
        REQUIRE(s.has_value());
        CHECK(s.value().mean[2] == 1.0f);
        CHECK(s.value().mean[0] == 0.0f);
    }

    SECTION("a float texture reads as stored")
    {
        auto const texels = cc::array<f32>{4.0f, 2.0f, -1.0f, 1.0f};
        auto const s = sv::summarize_texels(cc::span<f32 const>(texels).as_bytes(), sg::pixel_format::rgba32_float, 1, 1);
        REQUIRE(s.has_value());
        CHECK(s.value().mean == tg::vec4f(4.0f, 2.0f, -1.0f, 1.0f));
    }

    SECTION("a half-float texture reads as stored, a missing channel 0 and its alpha 1")
    {
        // 1.0 and 0.5, then 2.0 and -0.5.
        auto const texels = cc::array<u16>{0x3c00, 0x3800, 0x4000, 0xb800};
        auto const s = sv::summarize_texels(cc::span<u16 const>(texels).as_bytes(), sg::pixel_format::rg16_float, 2, 1);
        REQUIRE(s.has_value());
        CHECK(s.value().exact);
        CHECK(s.value().mean == tg::vec4f(1.5f, 0.0f, 0.0f, 1.0f));
        CHECK(s.value().min[1] == -0.5f);
        CHECK(s.value().max[0] == 2.0f);
    }

    SECTION("a level past the texel budget is read in row runs on a grid, which estimates the mean and bounds nothing")
    {
        // 1024 texels of 64 x 64: every other row, in runs of 16 texels 32 apart, so texel 20 of row 0 falls between two runs.
        auto texels = cc::vector<u8>::create_filled(64 * 64, u8(0));
        texels[20] = u8(255);
        auto const s
            = sv::summarize_texels(cc::span<u8 const>(texels).as_bytes(), sg::pixel_format::r8_unorm, 64, 64, 1024);
        REQUIRE(s.has_value());
        CHECK(!s.value().exact);
        CHECK(s.value().max[0] == 0.0f);

        auto const whole
            = sv::summarize_texels(cc::span<u8 const>(texels).as_bytes(), sg::pixel_format::r8_unorm, 64, 64);
        REQUIRE(whole.has_value());
        CHECK(whole.value().exact);
        CHECK(whole.value().max[0] == 1.0f);
    }

    SECTION("an integer format is not summarized")
    {
        auto const bytes = cc::vector<u8>::create_filled(16, u8(0));
        CHECK(!sv::summarize_texels(cc::span<u8 const>(bytes).as_bytes(), sg::pixel_format::rgba8_uint, 2, 2).has_value());
    }
}

TEST("sv - a texel budget below one run still ends, having read one run")
{
    // Sixteen texels are the shortest run an uncompressed scan reads, and four blocks a compressed one's.
    SECTION("uncompressed")
    {
        auto texels = cc::vector<u8>::create_filled(64 * 64, u8(0));
        texels[3] = u8(255);
        for (auto const budget : {isize(0), isize(1), isize(15)})
        {
            auto const s = sv::summarize_texels(cc::span<u8 const>(texels).as_bytes(), sg::pixel_format::r8_unorm, 64,
                                                64, budget);
            REQUIRE(s.has_value());
            CHECK(!s.value().exact);
            // The one run is the first row's first sixteen texels.
            CHECK(s.value().max[0] == 1.0f);
            CHECK(tg::abs(s.value().mean[0] - 1.0f / 16) < 1e-6f);
        }
    }

    SECTION("block-compressed, whose budget in blocks is a sixteenth of the texels'")
    {
        // 8 x 8 BC4 blocks of 0.8 throughout: endpoint 204, every index 0.
        auto blocks = cc::vector<u8>();
        for (auto i = 0; i < 64; ++i)
            for (auto const b : {u8(204), u8(0), u8(0), u8(0), u8(0), u8(0), u8(0), u8(0)})
                blocks.push_back(b);
        for (auto const budget : {isize(0), isize(16), isize(63)})
        {
            auto const s = sv::summarize_texels(cc::span<u8 const>(blocks).as_bytes(), sg::pixel_format::bc4_r_unorm,
                                                32, 32, budget);
            REQUIRE(s.has_value());
            CHECK(!s.value().exact);
            CHECK(tg::abs(s.value().mean[0] - 0.8f) < 1e-6f);
        }
    }
}

TEST("sv - a strided scan of a narrow tall level reaches every column in time")
{
    // 32 x 4096 within 1024 texels: every 64th row, one run of 16 texels each, which alternates between the row's two halves.
    // Column 20 alone is lit, so a scan that started every row at column 0 would see nothing.
    auto texels = cc::vector<u8>::create_filled(32 * 4096, u8(0));
    for (auto y = 0; y < 4096; ++y)
        texels[y * 32 + 20] = u8(255);
    auto const s
        = sv::summarize_texels(cc::span<u8 const>(texels).as_bytes(), sg::pixel_format::r8_unorm, 32, 4096, 1024);
    REQUIRE(s.has_value());
    CHECK(!s.value().exact);
    CHECK(s.value().max[0] == 1.0f);
    // Half the rows read the lit half, whose run holds one lit texel in sixteen.
    CHECK(tg::abs(s.value().mean[0] - 1.0f / 32) < 1e-6f);
}

TEST("sv - a texel that is not finite is skipped, leaving a finite summary that is never exact")
{
    auto const nan = cc::bit_cast<f32>(0x7fc00000u);
    auto const inf = cc::bit_cast<f32>(0x7f800000u);

    SECTION("f32")
    {
        auto const texels = cc::array<f32>{1, 2, 3, 1, nan, 0, 0, 1, 0, inf, 0, 1, 0, 0, -inf, 1};
        auto const s = sv::summarize_texels(cc::span<f32 const>(texels).as_bytes(), sg::pixel_format::rgba32_float, 4, 1);
        REQUIRE(s.has_value());
        CHECK(!s.value().exact);
        CHECK(all_finite(s.value().mean));
        CHECK(all_finite(s.value().min));
        CHECK(all_finite(s.value().max));
        CHECK(s.value().mean == tg::vec4f(1, 2, 3, 1));
    }

    SECTION("f16")
    {
        // 1.0, then a NaN, then an infinity, each in red.
        auto const texels = cc::array<u16>{0x3c00, 0, 0, 0x3c00, 0x7e00, 0, 0, 0x3c00, 0x7c00, 0, 0, 0x3c00};
        auto const s = sv::summarize_texels(cc::span<u16 const>(texels).as_bytes(), sg::pixel_format::rgba16_float, 3, 1);
        REQUIRE(s.has_value());
        CHECK(!s.value().exact);
        CHECK(all_finite(s.value().mean));
        CHECK(all_finite(s.value().max));
        CHECK(s.value().mean == tg::vec4f(1, 0, 0, 1));
    }

    SECTION("a level of nothing finite has no summary at all")
    {
        auto const texels = cc::array<f32>{nan};
        CHECK(!sv::summarize_texels(cc::span<f32 const>(texels).as_bytes(), sg::pixel_format::r32_float, 1, 1).has_value());
    }
}

TEST("sv - a block-compressed level is decoded as the sampler reads it, and never exact")
{
    SECTION("BC1, cropped to a level smaller than its block")
    {
        // One BC1 block in four-color mode: white then black endpoints, its left two columns white and its right two black.
        auto const block = cc::array<u8>{0xff, 0xff, 0x00, 0x00, 0x50, 0x50, 0x50, 0x50};
        auto const whole
            = sv::summarize_texels(cc::span<u8 const>(block).as_bytes(), sg::pixel_format::bc1_rgba_unorm, 4, 4);
        REQUIRE(whole.has_value());
        CHECK(!whole.value().exact);
        CHECK(whole.value().mean[0] == 0.5f);
        CHECK(whole.value().max[1] == 1.0f);
        CHECK(whole.value().mean[3] == 1.0f);

        // A 2 x 2 level samples the block's top-left corner alone.
        auto const corner
            = sv::summarize_texels(cc::span<u8 const>(block).as_bytes(), sg::pixel_format::bc1_rgba_unorm, 2, 2);
        REQUIRE(corner.has_value());
        CHECK(corner.value().mean[0] == 1.0f);
        CHECK(!corner.value().exact);
    }

    SECTION("BC4, unsigned and signed")
    {
        // The first endpoint everywhere, 204, which is 0.8 unsigned.
        auto const first = cc::array<u8>{204, 0, 0, 0, 0, 0, 0, 0};
        auto const u = sv::summarize_texels(cc::span<u8 const>(first).as_bytes(), sg::pixel_format::bc4_r_unorm, 4, 4);
        REQUIRE(u.has_value());
        CHECK(!u.value().exact);
        CHECK(tg::abs(u.value().mean[0] - 0.8f) < 1e-6f);
        CHECK(u.value().mean[1] == 0.0f);
        CHECK(u.value().mean[3] == 1.0f);

        // Endpoints 127 and -127, every index 1, so the second: -1 throughout, which a signed level keeps below zero.
        auto const second = cc::array<u8>{0x7f, 0x81, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24};
        auto const s = sv::summarize_texels(cc::span<u8 const>(second).as_bytes(), sg::pixel_format::bc4_r_snorm, 4, 4);
        REQUIRE(s.has_value());
        CHECK(!s.value().exact);
        CHECK(s.value().mean[0] == -1.0f);
        CHECK(s.value().max[0] == -1.0f);
    }

    SECTION("BC5, red from its first half and green from its second")
    {
        auto const block = cc::array<u8>{255, 0, 0, 0, 0, 0, 0, 0, 51, 0, 0, 0, 0, 0, 0, 0};
        auto const u = sv::summarize_texels(cc::span<u8 const>(block).as_bytes(), sg::pixel_format::bc5_rg_unorm, 4, 4);
        REQUIRE(u.has_value());
        CHECK(!u.value().exact);
        CHECK(tg::abs(u.value().mean[0] - 1.0f) < 1e-6f);
        CHECK(tg::abs(u.value().mean[1] - 0.2f) < 1e-6f);
        CHECK(u.value().mean[2] == 0.0f);

        // Signed, the same bytes read 127 as 1 in red, and 51 as 51 / 127 in green.
        auto const signed_block = cc::array<u8>{127, 0, 0, 0, 0, 0, 0, 0, 51, 0, 0, 0, 0, 0, 0, 0};
        auto const s
            = sv::summarize_texels(cc::span<u8 const>(signed_block).as_bytes(), sg::pixel_format::bc5_rg_snorm, 4, 4);
        REQUIRE(s.has_value());
        CHECK(tg::abs(s.value().mean[0] - 1.0f) < 1e-6f);
        CHECK(tg::abs(s.value().mean[1] - 51.0f / 127.0f) < 1e-6f);
    }

    SECTION("BC6H, as babel's decoder reads it")
    {
        // Mode 11: one region, both endpoints 512 in every channel, every index 0.
        auto const block = cc::array<u8>{0x03, 0x40, 0x00, 0x01, 0x04, 0x10, 0x40, 0x00,
                                         0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        auto const bytes = cc::span<u8 const>(block).as_bytes();
        auto const decoded = babel::bcn::decode_bc6h(babel::bcn::large_block(bytes.data()), false);
        auto const s = sv::summarize_texels(bytes, sg::pixel_format::bc6h_rgb_ufloat, 4, 4);
        REQUIRE(s.has_value());
        CHECK(!s.value().exact);
        CHECK(s.value().mean[0] > 0.0f);
        CHECK(s.value().mean == tg::vec4f(decoded.rgba[0], decoded.rgba[1], decoded.rgba[2], decoded.rgba[3]));
    }
}

TEST("sv - a texture's summary reads its smallest supplied level, exact only where that is the base")
{
    // A 2 x 2 base of black, and its 1 x 1 level of 0.2 red.
    auto pixels = cc::vector<byte>::create_filled(5 * 4, byte(0));
    pixels[16] = byte(51);
    pixels[19] = byte(255);
    for (auto i = 0; i < 4; ++i)
        pixels[i * 4 + 3] = byte(255);

    auto const base_only = sv::summarize_texture(
        sv::texture_data::create(cc::vector<byte>::create_copy_of(pixels), sg::pixel_format::rgba8_unorm, 2, 2));
    REQUIRE(base_only.has_value());
    CHECK(base_only.value().exact);
    CHECK(base_only.value().max[0] == 0.0f);

    // The smaller level estimates the mean, and its range need not bound the base's.
    auto const chain
        = sv::summarize_texture(sv::texture_data::create(cc::move(pixels), sg::pixel_format::rgba8_unorm, 2, 2, 2));
    REQUIRE(chain.has_value());
    CHECK(!chain.value().exact);
    CHECK(tg::abs(chain.value().mean[0] - 0.2f) < 1e-6f);
}

TEST("sv - a mesh attribute past the element cap is read in runs, and its summary is never exact")
{
    // 10000 elements, all 0 but 200 bright ones in the middle, under a cap of 1000.
    // That cap reads a run of 16 every 176 elements, so the bright stretch is wider than the gap between two runs.
    auto elements = cc::vector<f32>::create_filled(10000, 0.0f);
    for (auto i = 5000; i < 5200; ++i)
        elements[i] = 1.0f;
    auto const format = sv::attribute_format::of_scalar(sv::scalar_type::f32);
    auto const bytes = cc::span<f32 const>(elements).as_bytes();

    auto const capped = sv::summarize_elements(bytes, format, 10000, 1000);
    REQUIRE(capped.has_value());
    CHECK(!capped.value().exact);
    // The runs of 16 every so often still meet the bright run, and the mean estimates its share.
    CHECK(capped.value().max[0] == 1.0f);
    CHECK(capped.value().mean[0] > 0.0f);
    CHECK(capped.value().mean[0] < 0.05f);

    // All black under the cap is no proof of darkness, since elements between the runs went unread.
    auto const black = cc::vector<f32>::create_filled(10000, 0.0f);
    auto const unread = sv::summarize_elements(cc::span<f32 const>(black).as_bytes(), format, 10000, 1000);
    REQUIRE(unread.has_value());
    CHECK(!unread.value().exact);
    CHECK(unread.value().max[0] == 0.0f);

    // At the cap, every element is read.
    auto const whole = sv::summarize_elements(bytes, format, 10000, 10000);
    REQUIRE(whole.has_value());
    CHECK(whole.value().exact);
    CHECK(tg::abs(whole.value().mean[0] - 0.02f) < 1e-6f);
}

TEST("sv - a mesh attribute's summary reads every element, its missing lanes 0")
{
    auto const elements = cc::array<tg::vec3f>{tg::vec3f(1, 0, 2), tg::vec3f(3, -2, 2)};
    auto const s = sv::summarize_elements(cc::span<tg::vec3f const>(elements).as_bytes(),
                                          sv::attribute_format::of_vector(sv::scalar_type::f32, 3), 2);
    REQUIRE(s.has_value());
    CHECK(s.value().exact);
    CHECK(s.value().mean == tg::vec4f(2, -1, 2, 0));
    CHECK(s.value().min == tg::vec4f(1, -2, 2, 0));
    CHECK(s.value().max == tg::vec4f(3, 0, 2, 0));

    auto const ints = cc::array<i32>{1, 2};
    CHECK(!sv::summarize_elements(cc::span<i32 const>(ints).as_bytes(),
                                  sv::attribute_format::of_scalar(sv::scalar_type::i32), 2)
               .has_value());
}
