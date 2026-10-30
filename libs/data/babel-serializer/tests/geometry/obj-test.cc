#include <babel-serializer/geometry/obj.hh>
#include <clean-core/container/span.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
cc::span<byte const> as_bytes(cc::string_view s)
{
    return cc::span<byte const>(reinterpret_cast<byte const*>(s.data()), s.size());
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

    auto adapter = cc::span_read_stream_adapter(as_bytes(src));
    cc::read_stream stream = adapter;

    auto const m = babel::obj::read(stream).value();
    CHECK(m.positions.size() == 3);
    REQUIRE(m.faces.size() == 1);
    CHECK(m.faces[0].corner_count == 3);
}

TEST("obj - an MTL file's materials, as an exporter writes them")
{
    auto const src = cc::string_view(R"(# exported
Kd 9 9 9
newmtl default
	Ns 32
	d 1
	illum 2
	Kd 0.4 0.4 0.4
	Ks 0.7 0.7 0.7
	Ka 0.3 0.3 0.3

newmtl Surface glass
	d -halo 0.25
	Tr 0.75
	Kd 0.5
	Ke 1 2 3
	Tf spectral glass.rfl
	map_Kd C:\Users\someone\Desktop\walls floors_image0.jpg
	map_bump -bm 0.5 -clamp on -o 0.25 0.5 -s 2 2 2 -mm 0 1 textures/bump.png
	Pr 0.6
	sharpness 60
)");

    auto const parsed = babel::obj::read_materials(src);
    REQUIRE(parsed.has_value());
    auto const& ms = parsed.value();
    REQUIRE(ms.size() == 2);

    // the Kd before the first newmtl belongs to no material, and is dropped
    auto const& d = ms[0];
    CHECK(d.name == "default");
    REQUIRE(d.diffuse.has_value());
    CHECK(d.diffuse.value() == tg::vec3f(0.4f, 0.4f, 0.4f));
    CHECK(d.specular_exponent.value() == 32);
    CHECK(d.dissolve.value() == 1);
    CHECK(d.illumination_model.value() == 2);
    CHECK(!d.emissive.has_value());
    CHECK(!d.diffuse_map.has_value());

    auto const& g = ms[1];
    CHECK(g.name == "Surface glass");
    CHECK(g.dissolve.value() == 0.25f);
    CHECK(g.transparency.value() == 0.75f);
    CHECK(g.diffuse.value() == tg::vec3f(0.5f, 0.5f, 0.5f)); // one number is every channel
    CHECK(g.emissive.value() == tg::vec3f(1, 2, 3));
    CHECK(!g.transmission_filter.has_value()); // spectral is not modeled
    CHECK(g.roughness.value() == 0.6f);

    // the file name is the rest of the line, spaces and the exporting machine's absolute path included
    REQUIRE(g.diffuse_map.has_value());
    CHECK(g.diffuse_map.value().filename == "C:\\Users\\someone\\Desktop\\walls floors_image0.jpg");

    REQUIRE(g.bump_map.has_value());
    auto const& bump = g.bump_map.value();
    CHECK(bump.filename == "textures/bump.png");
    CHECK(bump.bump_multiplier == 0.5f);
    CHECK(bump.clamp);
    CHECK(bump.offset == tg::vec3f(0.25f, 0.5f, 0));
    CHECK(bump.scale == tg::vec3f(2, 2, 2));
}

TEST("obj - a malformed MTL statement fails with its line")
{
    auto const bad_number = babel::obj::read_materials(cc::string_view("newmtl a\nNs lots\n"));
    REQUIRE(bad_number.has_error());
    CHECK(cc::format("{}", bad_number.error()).contains("line 2"));

    auto const bad_option = babel::obj::read_materials(cc::string_view("newmtl a\nmap_Kd -nonsense 1 a.png\n"));
    CHECK(bad_option.has_error());

    auto const no_file = babel::obj::read_materials(cc::string_view("newmtl a\nmap_Kd -clamp on\n"));
    CHECK(no_file.has_error());

    CHECK(babel::obj::read_materials(cc::string_view("")).value().empty());
}
