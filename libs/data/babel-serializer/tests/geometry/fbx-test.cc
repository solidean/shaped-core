#include "fbx_fixtures.hh"

#include <babel-serializer/geometry/fbx.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// The scene under test is described once, in fbx_fixtures.hh; these tests pin what each part of it reads back as.

namespace
{
namespace bf = babel::fbx;

[[nodiscard]] bool near(f32 a, f32 b)
{
    auto const d = a - b;
    return d < 1e-5f && d > -1e-5f;
}

[[nodiscard]] bool near(tg::pos3f a, tg::pos3f b)
{
    return near(a[0], b[0]) && near(a[1], b[1]) && near(a[2], b[2]);
}

[[nodiscard]] bf::node const* node_named(bf::data const& doc, cc::string_view name)
{
    for (auto const& n : doc.nodes)
        if (n.name == name)
            return &n;
    return nullptr;
}

[[nodiscard]] cc::string_view material_name(bf::data const& doc, bf::material_index i)
{
    auto const* const m = doc.find(i);
    return m != nullptr ? cc::string_view(m->name) : cc::string_view("<none>");
}
} // namespace

TEST("fbx - reads the file info and settings as declared, converting nothing")
{
    auto const text = babel::test::fbx_scene({.up_axis = 2, .front_axis = 1, .front_sign = -1, .unit_scale_factor = 100});
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(text)));

    CHECK(doc.info.version == 7400);
    CHECK(doc.info.encoding == bf::encoding::ascii);
    CHECK(doc.info.creator == "shaped-core test");

    // 3ds Max's convention: Z up, -Y front — which is right-handed, since X cross Z is -Y.
    CHECK(doc.settings.right == bf::axis::positive_x);
    CHECK(doc.settings.up == bf::axis::positive_z);
    CHECK(doc.settings.front == bf::axis::negative_y);
    CHECK(!doc.settings.is_left_handed());

    // UnitScaleFactor is centimeters per unit, so 100 is a meter scene.
    CHECK(near(f32(doc.settings.unit_meters), 1.0f));

    // Reported, not applied: the parent still stands at the file's own (10, 0, 0).
    auto const* const parent = node_named(doc, "parent");
    REQUIRE(parent != nullptr);
    CHECK(near(parent->node_to_world.transform(tg::pos3f::zero), tg::pos3f(10, 0, 0)));
}

TEST("fbx - a centimeter scene reports 0.01 meters per unit")
{
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene({.unit_scale_factor = 1}))));
    CHECK(near(f32(doc.settings.unit_meters), 0.01f));
    CHECK(doc.settings.up == bf::axis::positive_y);
}

TEST("fbx - a left-handed axis set is detected")
{
    // Y up with +X right and -Z front: X cross Y is +Z, so a -Z front mirrors.
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene({.front_sign = -1}))));
    CHECK(doc.settings.front == bf::axis::negative_z);
    CHECK(doc.settings.is_left_handed());
}

TEST("fbx - nodes come back parents first, with composed world transforms")
{
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene())));

    REQUIRE(doc.nodes.size() == 4);
    CHECK(doc.nodes[0].parent == bf::node_index::invalid);
    for (auto i = isize(1); i < doc.nodes.size(); ++i)
    {
        auto const parent = isize(int(doc.nodes[i].parent));
        CHECK(parent >= 0);
        CHECK(parent < i);
    }

    auto const* const parent = node_named(doc, "parent");
    auto const* const child = node_named(doc, "child");
    REQUIRE(parent != nullptr);
    REQUIRE(child != nullptr);
    CHECK(doc.find(child->parent) == parent);
    CHECK(doc.children_of(*parent).size() == 1);

    // The child's (0, 5, 0) is in its parent's space, which is scaled by 2 and moved to (10, 0, 0).
    CHECK(near(child->node_to_world.transform(tg::pos3f::zero), tg::pos3f(10, 10, 0)));
    CHECK(near(child->local.translation[1], 5.0f));
    CHECK(near(parent->local.scale[0], 2.0f));
    CHECK(child->inherit == bf::inherit_mode::normal);
    CHECK(!child->has_geometry_transform);
}

TEST("fbx - one mesh instanced by two nodes, each with its own material slots")
{
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene())));

    REQUIRE(doc.meshes.size() == 1);
    auto const* const parent = node_named(doc, "parent");
    auto const* const child = node_named(doc, "child");
    REQUIRE(parent != nullptr);
    REQUIRE(child != nullptr);

    // Instancing: both nodes name the same mesh rather than a copy each.
    CHECK(parent->mesh == bf::mesh_index(0));
    CHECK(child->mesh == bf::mesh_index(0));

    // FBX connects materials to the NODE, so the same face slot draws differently on the two instances.
    auto const parent_slots = doc.materials_of(*parent);
    auto const child_slots = doc.materials_of(*child);
    REQUIRE(parent_slots.size() == 2);
    REQUIRE(child_slots.size() == 2);
    CHECK(material_name(doc, parent_slots[0]) == "red");
    CHECK(material_name(doc, parent_slots[1]) == "glow");
    CHECK(material_name(doc, child_slots[0]) == "glow");
    CHECK(material_name(doc, child_slots[1]) == "red");
}

TEST("fbx - polygons, per-corner attributes and per-face material slots")
{
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene())));
    REQUIRE(doc.meshes.size() == 1);
    auto const& m = doc.meshes[0];

    CHECK(m.name == "shape");
    REQUIRE(m.faces.size() == 2);
    CHECK(m.faces[0].corner_count == 4);
    CHECK(m.faces[1].corner_count == 3);
    CHECK(m.corner_count() == 7);

    // Positions are per control point, reached through a per-corner index.
    CHECK(m.positions.values.size() == 5);
    CHECK(near(m.positions.at(5), tg::pos3f(2, 0, 0))); // the triangle's second corner names control point 4

    CHECK(m.normals.exists());
    CHECK(m.normals.indices.size() == 7);
    CHECK(!m.tangents.exists());

    REQUIRE(m.uv_sets.size() == 1);
    CHECK(m.uv_sets[0].name == "map1");
    CHECK(m.uv_sets[0].uvs.values.size() == 4);
    CHECK(near(m.uv_sets[0].uvs.at(2)[1], 1.0f)); // FBX's V points up the image, kept as written

    REQUIRE(m.face_materials.size() == 2);
    CHECK(m.material_slot_of(0) == 0);
    CHECK(m.material_slot_of(1) == 1);
}

TEST("fbx - triangulation splits polygons and keeps every corner's attributes")
{
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene()), {.triangulate = true}));
    REQUIRE(doc.meshes.size() == 1);
    auto const& m = doc.meshes[0];

    REQUIRE(m.faces.size() == 3);
    for (auto const& f : m.faces)
        CHECK(f.corner_count == 3);
    CHECK(m.corner_count() == 9);
    CHECK(m.normals.indices.size() == 9);
    CHECK(m.uv_sets[0].uvs.indices.size() == 9);

    // The quad's two triangles both keep slot 0; the triangle keeps slot 1.
    REQUIRE(m.face_materials.size() == 3);
    CHECK(m.material_slot_of(0) == 0);
    CHECK(m.material_slot_of(1) == 0);
    CHECK(m.material_slot_of(2) == 1);

    // Copied rather than re-indexed, so the value arrays are untouched.
    CHECK(m.positions.values.size() == 5);
}

TEST("fbx - materials are property bags with textures connected by property name")
{
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene())));
    REQUIRE(doc.materials.size() == 2);

    auto const& red = doc.materials[0];
    CHECK(red.name == "red");
    CHECK(red.shading_model == "phong");
    auto const* const shininess = red.find_property("Shininess");
    REQUIRE(shininess != nullptr);
    CHECK(near(shininess->value[0], 20.0f));
    CHECK(red.find_property("NoSuchProperty") == nullptr);

    auto const* const texture = doc.find(red.find_texture("DiffuseColor"));
    REQUIRE(texture != nullptr);
    CHECK(texture->name == "wood");
    CHECK(texture->relative_filename == "textures\\wood.png");
    CHECK(texture->absolute_filename == "C:/somewhere/textures/wood.png");
    CHECK(texture->content.empty());
    CHECK(texture->is_file);
    CHECK(red.find_texture("NormalMap") == bf::texture_index::invalid);

    auto const& glow = doc.materials[1];
    CHECK(glow.shading_model == "lambert");
    auto const* const emissive = glow.find_property("EmissiveColor");
    REQUIRE(emissive != nullptr);
    CHECK(emissive->type == bf::property_type::color);
    CHECK(near(emissive->value[1], 1.0f));
    CHECK(near(glow.find_property("EmissiveFactor")->value[0], 4.0f));
}

TEST("fbx - a light reads its type, decay, color and intensity over 100")
{
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene())));
    REQUIRE(doc.lights.size() == 1);

    auto const& l = doc.lights[0];
    CHECK(l.type == bf::light_type::point);
    CHECK(l.decay == bf::light_decay::quadratic);
    CHECK(near(l.color[1], 0.5f));
    CHECK(near(l.intensity, 2.5f)); // FBX's Intensity is a percentage

    auto const* const lamp = node_named(doc, "lamp");
    REQUIRE(lamp != nullptr);
    CHECK(lamp->light == bf::light_index(0));
    CHECK(near(lamp->node_to_world.transform(tg::pos3f::zero), tg::pos3f(0, 0, 3)));
}

TEST("fbx - a clean file yields no issues")
{
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene())));
    CHECK(!doc.has_issues());
    CHECK(doc.issue_report().empty());
}

TEST("fbx - anything that is not FBX is an error, not an empty scene")
{
    CHECK(bf::read(cc::string_view("solid not an fbx at all\n")).has_error());
    CHECK(bf::read(cc::span<byte const>()).has_error());

    // A binary header cut off right after its magic.
    CHECK(bf::read(cc::string_view("Kaydara FBX Binary  \0\x1a\0", 23)).has_error());
}

// The real thing, when a developer has fetched it with extern/bistro/fetch-bistro.py.
// Parsing the 114 MB exterior takes seconds rather than milliseconds, so only the thorough run does it.
TEST("fbx - Bistro exterior reads when present")
{
    if (!nx::is_thorough())
        SKIP("thorough only: parses a 114 MB file");

    auto adapter = cc::file_read_stream_adapter::open(BABEL_TEST_BISTRO_DIR "/BistroExterior.fbx");
    if (adapter.has_error())
        SKIP("Bistro is not fetched; uv run extern/bistro/fetch-bistro.py installs it");

    auto stream = adapter.value().stream();
    cc::read_stream plain = cc::move(stream);
    auto const doc = REQUIRED_VALUE(bf::read(plain, {.triangulate = true}));

    CHECK(doc.meshes.size() == 1296);
    CHECK(doc.materials.size() == 132);
    CHECK(doc.lights.size() == 1);
    CHECK(near(f32(doc.settings.unit_meters), 1.0f));
    CHECK(doc.settings.up == bf::axis::positive_y);
}
