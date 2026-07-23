#include <babel-serializer/geometry/obj.hh>
#include <clean-core/container/span.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <nexus/test.hh>

namespace
{
cc::span<cc::byte const> as_bytes(cc::string_view s)
{
    return cc::span<cc::byte const>(reinterpret_cast<cc::byte const*>(s.data()), s.size());
}
} // namespace

TEST("obj - basic attributes and a triangle")
{
    auto const src = cc::string_view(R"(
# a single triangle
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
f 1/1/1 2/2/1 3/3/1
)");

    auto const m = babel::obj::read(src).value();

    REQUIRE(m.positions.size() == 3);
    REQUIRE(m.texcoords.size() == 3);
    REQUIRE(m.normals.size() == 1);

    CHECK(m.positions[1][0] == 1);
    CHECK(m.texcoords[2][1] == 1);
    CHECK(m.normals[0][2] == 1);

    REQUIRE(m.faces.size() == 1);
    CHECK(m.faces[0].first_corner == 0);
    CHECK(m.faces[0].corner_count == 3);

    REQUIRE(m.corners.size() == 3);
    CHECK(m.corners[0].position == 0);
    CHECK(m.corners[0].texcoord == 0);
    CHECK(m.corners[0].normal == 0);
    CHECK(m.corners[2].position == 2);
    CHECK(m.corners[2].texcoord == 2);
    CHECK(m.corners[2].normal == 0);
}

TEST("obj - polygon face preserved (not triangulated)")
{
    auto const src = cc::string_view(R"(
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
f 1 2 3 4
)");

    auto const m = babel::obj::read(src).value();
    REQUIRE(m.faces.size() == 1);
    CHECK(m.faces[0].corner_count == 4);
    REQUIRE(m.corners.size() == 4);
    CHECK(m.corners[3].position == 3);
    CHECK(m.corners[3].texcoord == -1); // absent
    CHECK(m.corners[3].normal == -1);   // absent
}

TEST("obj - corner index forms")
{
    auto const src = cc::string_view(R"(
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vn 0 0 1
f 1 2 3
f 1/1 2/1 3/1
f 1//1 2//1 3//1
f 1/1/1 2/1/1 3/1/1
)");

    auto const m = babel::obj::read(src).value();
    REQUIRE(m.faces.size() == 4);

    // f 1 2 3 -> position only
    CHECK(m.corners[m.faces[0].first_corner].position == 0);
    CHECK(m.corners[m.faces[0].first_corner].texcoord == -1);
    CHECK(m.corners[m.faces[0].first_corner].normal == -1);

    // f 1/1 -> position + texcoord
    CHECK(m.corners[m.faces[1].first_corner].texcoord == 0);
    CHECK(m.corners[m.faces[1].first_corner].normal == -1);

    // f 1//1 -> position + normal
    CHECK(m.corners[m.faces[2].first_corner].texcoord == -1);
    CHECK(m.corners[m.faces[2].first_corner].normal == 0);

    // f 1/1/1 -> all three
    CHECK(m.corners[m.faces[3].first_corner].texcoord == 0);
    CHECK(m.corners[m.faces[3].first_corner].normal == 0);
}

TEST("obj - negative (relative) indices resolve from the end")
{
    auto const src = cc::string_view(R"(
v 0 0 0
v 1 0 0
v 0 1 0
f -3 -2 -1
)");

    auto const m = babel::obj::read(src).value();
    REQUIRE(m.corners.size() == 3);
    CHECK(m.corners[0].position == 0); // -3 of 3 -> index 0
    CHECK(m.corners[1].position == 1);
    CHECK(m.corners[2].position == 2); // -1 -> last
}

TEST("obj - group / object / material face ranges")
{
    auto const src = cc::string_view(R"(
mtllib scene.mtl
o cube
g top
usemtl red
v 0 0 0
v 1 0 0
v 0 1 0
f 1 2 3
f 1 2 3
usemtl blue
f 1 2 3
)");

    auto const m = babel::obj::read(src).value();

    REQUIRE(m.material_libraries.size() == 1);
    CHECK(m.material_libraries[0] == "scene.mtl");

    REQUIRE(m.materials.size() == 2);
    CHECK(m.materials[0].name == "red");
    CHECK(m.materials[0].first_face == 0);
    CHECK(m.materials[0].face_count == 2);
    CHECK(m.materials[1].name == "blue");
    CHECK(m.materials[1].first_face == 2);
    CHECK(m.materials[1].face_count == 1);

    REQUIRE(m.objects.size() == 1);
    CHECK(m.objects[0].name == "cube");
    CHECK(m.objects[0].first_face == 0);
    CHECK(m.objects[0].face_count == 3);

    REQUIRE(m.groups.size() == 1);
    CHECK(m.groups[0].name == "top");
    CHECK(m.groups[0].face_count == 3);
}

TEST("obj - comments and blank lines ignored; unknown directives skipped")
{
    auto const src = cc::string_view(R"(
# comment

v 0 0 0
s 1
v 1 0 0
v 0 1 0
f 1 2 3
)");

    auto const m = babel::obj::read(src).value();
    CHECK(m.positions.size() == 3); // 's 1' skipped, blank/comment ignored
    CHECK(m.faces.size() == 1);
}

TEST("obj - reads through the read_stream overload")
{
    auto const src = cc::string_view("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    cc::span_read_stream_adapter adapter{as_bytes(src)};
    cc::read_stream stream = adapter;

    auto const m = babel::obj::read(stream).value();
    CHECK(m.positions.size() == 3);
    REQUIRE(m.faces.size() == 1);
    CHECK(m.faces[0].corner_count == 3);
}
