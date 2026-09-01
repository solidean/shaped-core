#include <babel-serializer/geometry/stl.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
void push_le_u16(cc::vector<byte>& out, u16 value)
{
    out.push_back(byte(u8(value & 0xFFu)));
    out.push_back(byte(u8((value >> 8) & 0xFFu)));
}

void push_le_u32(cc::vector<byte>& out, u32 value)
{
    for (auto k = 0; k < 4; ++k)
        out.push_back(byte(u8((value >> (8 * k)) & 0xFFu)));
}

void push_le_f32(cc::vector<byte>& out, f32 value)
{
    auto bits = u32(0);
    cc::memcpy(&bits, &value, sizeof(bits));
    push_le_u32(out, bits);
}

/// A binary .stl of one triangle, whose 80-byte header deliberately opens with "solid" — the trap that makes the
/// leading keyword useless for detection.
[[nodiscard]] cc::vector<byte> binary_triangle(u16 attribute = 0)
{
    auto out = cc::vector<byte>();

    constexpr cc::string_view header = "solid exported by a tool that writes binary";
    for (auto const c : header)
        out.push_back(byte(c));
    while (out.size() < 80)
        out.push_back(byte(0));

    push_le_u32(out, 1);

    for (auto const v : {0.0f, 0.0f, 1.0f}) // normal
        push_le_f32(out, v);
    for (auto const v : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f})
        push_le_f32(out, v);
    push_le_u16(out, attribute);

    return out;
}

constexpr cc::string_view ascii_two_triangles = R"stl(solid quad
  facet normal 0 0 1
    outer loop
      vertex 0 0 0
      vertex 1 0 0
      vertex 1 1 0
    endloop
  endfacet
  facet normal 0 0 0
    outer loop
      vertex 0 0 0
      vertex 1 1 0
      vertex 0 1 0
    endloop
  endfacet
endsolid quad
)stl";
} // namespace

TEST("babel::stl - a binary file is detected by its SIZE, not by its leading keyword")
{
    auto const bytes = binary_triangle(7);

    // The header is free-form vendor text and this one says "solid"; only the size arithmetic tells the truth.
    CHECK(babel::stl::detect_container(bytes) == babel::stl::container::binary);

    auto const m = babel::stl::read(cc::span<byte const>(bytes));
    REQUIRE(m.has_value());
    CHECK(m.value().source == babel::stl::container::binary);

    // A binary header is not a name, so nothing is invented for it.
    CHECK(m.value().name == "");

    REQUIRE(m.value().triangle_count() == 1);
    CHECK(m.value().normals[0] == tg::vec3f(0, 0, 1));
    CHECK(m.value().positions[0] == tg::pos3f(0, 0, 0));
    CHECK(m.value().positions[1] == tg::pos3f(1, 0, 0));
    CHECK(m.value().positions[2] == tg::pos3f(0, 1, 0));

    // The attribute field is reserved by the format and abused by exporters, so it is mirrored rather than dropped.
    REQUIRE(m.value().attribute_counts.size() == 1);
    CHECK(m.value().attribute_counts[0] == 7);
}

TEST("babel::stl - a truncated binary file is an error, not an empty solid")
{
    auto bytes = binary_triangle();
    bytes.pop_back();

    auto const r = babel::stl::read(cc::span<byte const>(bytes));
    REQUIRE(r.has_error());
    CHECK(r.error().to_string().contains("truncated"));

    // The interesting part: the ascii parser would have READ these bytes happily.
    // Its header opens with `solid`, nothing after it spells `facet`, and the answer would be a valid, empty solid —
    // which is why the truncation is caught before the fall-through rather than after it.
    CHECK(babel::stl::detect_container(bytes) == babel::stl::container::ascii);
}

TEST("babel::stl - an ascii file mirrors its facets in order")
{
    CHECK(babel::stl::detect_container(ascii_two_triangles.as_bytes()) == babel::stl::container::ascii);

    auto const m = babel::stl::read(ascii_two_triangles);
    REQUIRE(m.has_value());
    CHECK(m.value().source == babel::stl::container::ascii);
    CHECK(m.value().name == "quad");

    REQUIRE(m.value().triangle_count() == 2);
    CHECK(m.value().positions.size() == 6);
    CHECK(m.value().positions[2] == tg::pos3f(1, 1, 0));
    CHECK(m.value().positions[5] == tg::pos3f(0, 1, 0));

    // A zero normal is the file saying "derive it from the winding"; the reader computes nothing and says so.
    CHECK(m.value().normals[0] == tg::vec3f(0, 0, 1));
    CHECK(m.value().normals[1] == tg::vec3f(0, 0, 0));

    // Ascii has no attribute field at all, so the array stays empty rather than being filled with zeros.
    CHECK(m.value().attribute_counts.empty());
}

TEST("babel::stl - a malformed ascii facet fails with its line number")
{
    // Two vertices where a triangle needs three.
    constexpr cc::string_view short_facet = R"stl(solid s
facet normal 0 0 1
outer loop
vertex 0 0 0
vertex 1 0 0
endloop
endfacet
endsolid s
)stl";
    auto const r = babel::stl::read(short_facet);
    REQUIRE(r.has_error());
    CHECK(r.error().to_string().contains("line 7"));

    // A facet the file never closes.
    constexpr cc::string_view unclosed = R"stl(solid s
facet normal 0 0 1
outer loop
vertex 0 0 0
)stl";
    CHECK(babel::stl::read(unclosed).has_error());

    // A vertex outside any facet has no triangle to belong to.
    CHECK(babel::stl::read("solid s\nvertex 0 0 0\n").has_error());

    // A facet whose normal is not three numbers.
    CHECK(babel::stl::read("solid s\nfacet normal 0 0\n").has_error());
}

TEST("babel::stl - a facet that never ends cannot desynchronize the normals from the triangles")
{
    // The `endfacet` that would have caught the short facet never arrives, so without a nesting check this parses to
    // two normals and five positions — and `normals[i]` then describes a different triangle than `positions[3i]`.
    constexpr cc::string_view missing_endfacet = R"stl(solid s
facet normal 0 0 1
outer loop
vertex 0 0 0
vertex 1 0 0
facet normal 0 1 0
outer loop
vertex 0 0 0
vertex 1 0 0
vertex 0 1 0
endloop
endfacet
endsolid s
)stl";
    auto const r = babel::stl::read(missing_endfacet);
    REQUIRE(r.has_error());
    CHECK(r.error().to_string().contains("line 6"));

    // The well-formed version of the same file is two triangles, with the pairing intact.
    constexpr cc::string_view closed = R"stl(solid s
facet normal 0 0 1
outer loop
vertex 0 0 0
vertex 1 0 0
vertex 0 1 0
endloop
endfacet
facet normal 0 1 0
outer loop
vertex 0 0 0
vertex 1 0 0
vertex 0 0 1
endloop
endfacet
endsolid s
)stl";
    auto const ok = babel::stl::read(closed);
    REQUIRE(ok.has_value());
    CHECK(ok.value().triangle_count() == 2);
    CHECK(ok.value().normals.size() == 2);
    CHECK(ok.value().positions.size() == 6);
}

TEST("babel::stl - an empty solid is a valid file with nothing in it")
{
    auto const m = babel::stl::read("solid empty\nendsolid empty\n");
    REQUIRE(m.has_value());
    CHECK(m.value().is_empty());
    CHECK(m.value().triangle_count() == 0);
    CHECK(m.value().name == "empty");
}
