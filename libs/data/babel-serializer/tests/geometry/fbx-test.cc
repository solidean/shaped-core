#include "fbx_fixtures.hh"

#include <babel-serializer/geometry/fbx.hh>
#include <clean-core/common/time.hh>
#include <clean-core/math/bit.hh> // cc::bit_cast
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/async_thread_pool.hh>
#include <nexus/async-test.hh>
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

template <class T>
[[nodiscard]] bool same_attribute(bf::corner_attribute<T> const& a, bf::corner_attribute<T> const& b)
{
    if (a.values.size() != b.values.size() || a.indices.size() != b.indices.size())
        return false;
    for (auto i = isize(0); i < a.values.size(); ++i)
        if (a.values[i] != b.values[i])
            return false;
    for (auto i = isize(0); i < a.indices.size(); ++i)
        if (a.indices[i] != b.indices[i])
            return false;
    return true;
}

/// The first place two reads of one file differ, or empty when they agree on everything a reader reads.
/// Exact, floats included: both reads run the same arithmetic on the same input, whatever thread ran it.
[[nodiscard]] cc::string first_difference(bf::data const& a, bf::data const& b)
{
    if (a.issue_report() != b.issue_report())
        return cc::format("issues:\n{}\nagainst\n{}", a.issue_report(), b.issue_report());
    if (a.nodes.size() != b.nodes.size() || a.meshes.size() != b.meshes.size() || a.materials.size() != b.materials.size()
        || a.textures.size() != b.textures.size() || a.lights.size() != b.lights.size())
        return "the element counts";
    if (a.node_children.size() != b.node_children.size() || a.node_materials.size() != b.node_materials.size())
        return "the flattened node runs";

    for (auto i = isize(0); i < a.nodes.size(); ++i)
    {
        auto const& x = a.nodes[i];
        auto const& y = b.nodes[i];
        if (x.name != y.name || x.parent != y.parent || x.mesh != y.mesh || x.light != y.light
            || x.first_child != y.first_child || x.child_count != y.child_count || x.first_material != y.first_material
            || x.material_count != y.material_count || x.local.translation != y.local.translation
            || x.node_to_world.transform(tg::pos3f(1, 2, 3)) != y.node_to_world.transform(tg::pos3f(1, 2, 3)))
            return cc::format("node {} '{}'", i, x.name);
    }
    for (auto i = isize(0); i < a.meshes.size(); ++i)
    {
        auto const& x = a.meshes[i];
        auto const& y = b.meshes[i];
        auto same = x.name == y.name && x.faces.size() == y.faces.size()
                 && x.face_materials.size() == y.face_materials.size() && x.face_holes.size() == y.face_holes.size()
                 && x.uv_sets.size() == y.uv_sets.size() && same_attribute(x.positions, y.positions)
                 && same_attribute(x.normals, y.normals);
        for (auto f = isize(0); same && f < x.faces.size(); ++f)
            same = x.faces[f].first_corner == y.faces[f].first_corner
                && x.faces[f].corner_count == y.faces[f].corner_count && x.material_slot_of(f) == y.material_slot_of(f);
        for (auto s = isize(0); same && s < x.uv_sets.size(); ++s)
            same = x.uv_sets[s].name == y.uv_sets[s].name && same_attribute(x.uv_sets[s].uvs, y.uv_sets[s].uvs);
        if (!same)
            return cc::format("mesh {} '{}'", i, x.name);
    }
    for (auto i = isize(0); i < a.materials.size(); ++i)
        if (a.materials[i].name != b.materials[i].name
            || a.materials[i].properties.size() != b.materials[i].properties.size()
            || a.materials[i].defaults.size() != b.materials[i].defaults.size()
            || a.materials[i].textures.size() != b.materials[i].textures.size())
            return cc::format("material {} '{}'", i, a.materials[i].name);
    for (auto i = isize(0); i < a.textures.size(); ++i)
        if (a.textures[i].name != b.textures[i].name || a.textures[i].relative_filename != b.textures[i].relative_filename
            || a.textures[i].content.size() != b.textures[i].content.size())
            return cc::format("texture {} '{}'", i, a.textures[i].name);
    return {};
}

[[nodiscard]] cc::pinned_data<byte const> pinned_copy(cc::span<byte const> bytes)
{
    return cc::make_pinned_data(cc::vector<byte>::create_copy_of(bytes));
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
    CHECK(texture->content.size() == 0);
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

TEST("fbx - material_of bounds the slot lookup, and is invalid where the node has no material there")
{
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene())));
    auto const* const parent = node_named(doc, "parent");
    auto const* const child = node_named(doc, "child");
    REQUIRE(parent != nullptr);
    REQUIRE(child != nullptr);
    auto const& m = doc.meshes[0];
    CHECK(material_name(doc, doc.material_of(*parent, m, 1)) == "glow");
    CHECK(material_name(doc, doc.material_of(*child, m, 1)) == "red");

    // The binary triangle's node has no materials at all, so slot 0 is past an empty list.
    auto const bytes = babel::test::fbx_binary_scene();
    auto const bare = REQUIRED_VALUE(bf::read(cc::span<byte const>(bytes)));
    auto const* const tri = node_named(bare, "tri");
    REQUIRE(tri != nullptr);
    CHECK(bare.materials_of(*tri).empty());
    CHECK(bare.material_of(*tri, bare.meshes[0], 0) == bf::material_index::invalid);
}

TEST("fbx - instances listing different numbers of material slots are an issue")
{
    // A third material on the child: three slots, where the parent has two.
    // ufbx pads a node with fewer slots than the mesh uses, so only an instance listing MORE disagrees.
    auto const text
        = babel::test::fbx_scene_with("\tC: \"OO\",400,301\n", "\tC: \"OO\",400,301\n\tC: \"OO\",402,301\n");
    auto patched = cc::string(text);
    [[maybe_unused]] auto const added
        = patched.replace_first("\tTexture: 500,", "\tMaterial: 402, \"Material::blue\", \"\" {\n\t\tShadingModel: "
                                                   "\"phong\"\n\t}\n\tTexture: 500,");
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(patched)));
    REQUIRE(doc.materials.size() == 3);
    CHECK(doc.has_issue_of(bf::issue_kind::unsupported));
    CHECK(cc::string_view(doc.issue_report()).contains("different numbers of material slots"));
}

TEST("fbx - hole faces are carried, and triangulation drops them")
{
    auto const holes = cc::string_view("\t\tLayerElementHole: 0 {\n"
                                       "\t\t\tMappingInformationType: \"ByPolygon\"\n"
                                       "\t\t\tReferenceInformationType: \"Direct\"\n"
                                       "\t\t\tHole: *2 {\n\t\t\t\ta: 0,1\n\t\t\t}\n"
                                       "\t\t}\n"
                                       "\t\tLayer: 0 {\n");
    auto const text = babel::test::fbx_scene_with("\t\tLayer: 0 {\n", holes);

    auto const raw = REQUIRED_VALUE(bf::read(cc::string_view(text)));
    auto const& m = raw.meshes[0];
    REQUIRE(m.face_holes.size() == 2);
    CHECK(!m.is_hole(0));
    CHECK(m.is_hole(1));

    // The quad's two triangles remain; the triangle was the hole.
    auto const triangulated = REQUIRED_VALUE(bf::read(cc::string_view(text), {.triangulate = true}));
    CHECK(triangulated.meshes[0].faces.size() == 2);
    CHECK(triangulated.meshes[0].face_holes.empty());

    // A mesh without holes keeps the flags empty.
    auto const clean = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene())));
    CHECK(clean.meshes[0].face_holes.empty());
    CHECK(!clean.meshes[0].is_hole(1));
}

TEST("fbx - a LOD group is an issue naming it")
{
    auto const text = babel::test::fbx_scene_with("\tModel: 302,", "\tNodeAttribute: 210, \"NodeAttribute::levels\", "
                                                                   "\"LodGroup\" {\n\t\tTypeFlags: \"LodGroup\"\n\t}\n"
                                                                   "\tModel: 302,");
    auto patched = cc::string(text);
    [[maybe_unused]] auto const connected
        = patched.replace_first("\tC: \"OO\",200,302\n", "\tC: \"OO\",200,302\n\tC: \"OO\",210,301\n");
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(patched)));
    CHECK(cc::string_view(doc.issue_report()).contains("LOD group 'levels'"));
}

TEST("fbx - an authoring unit outside a sane range falls back to the scene's, with an issue")
{
    auto const text
        = babel::test::fbx_scene_with("\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n",
                                      "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
                                      "\t\tP: \"OriginalUnitScaleFactor\", \"double\", \"Number\", \"\",0\n");
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(text)));
    CHECK(near(f32(doc.settings.original_unit_meters), 0.01f));
    CHECK(cc::string_view(doc.issue_report()).contains("authoring tool's unit"));
}

TEST("fbx - a mesh whose every face triangulation drops keeps no values either")
{
    // One line face: two corners, which triangulation drops, leaving no corners at all.
    auto const text = babel::test::fbx_scene_with("a: 0,1,2,-4,1,4,-3", "a: 0,-2");
    auto const raw = REQUIRED_VALUE(bf::read(cc::string_view(text)));
    REQUIRE(raw.meshes[0].corner_count() == 2);

    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(text), {.triangulate = true}));
    auto const& m = doc.meshes[0];
    CHECK(m.faces.empty());
    CHECK(!m.positions.exists());
    CHECK(m.positions.values.empty());
    CHECK(m.normals.values.empty());
}

TEST("fbx - anything that is not FBX is an error, not an empty scene")
{
    CHECK(bf::read(cc::string_view("solid not an fbx at all\n")).has_error());
    CHECK(bf::read(cc::span<byte const>()).has_error());

    // A binary header cut off right after its magic.
    CHECK(bf::read(cc::string_view("Kaydara FBX Binary  \0\x1a\0", 23)).has_error());
}

TEST("fbx - a binary file reads through its compressed arrays")
{
    auto const bytes = babel::test::fbx_binary_scene();
    auto const doc = REQUIRED_VALUE(bf::read(cc::span<byte const>(bytes)));

    CHECK(doc.info.encoding == bf::encoding::binary);
    CHECK(doc.info.version == 7400);
    REQUIRE(doc.meshes.size() == 1);
    CHECK(doc.meshes[0].corner_count() == 3);
    CHECK(near(doc.meshes[0].positions.at(1), tg::pos3f(1, 0, 0)));

    auto const* const tri = node_named(doc, "tri");
    REQUIRE(tri != nullptr);
    CHECK(near(tri->node_to_world.transform(tg::pos3f::zero), tg::pos3f(0, 0, 5)));
}

TEST("fbx - an array stating more than the memory ceiling allows is an error, not an allocation")
{
    // A few hundred bytes claiming two billion vertices: ufbx sizes its buffer from the stated count.
    auto const bytes = babel::test::fbx_binary_scene(0x7ffffff0);
    auto const refused = bf::read(cc::span<byte const>(bytes));
    REQUIRE(refused.has_error());
    // Where `size_t` is 32 bits (wasm), the stated size overflows it, and ufbx's allocator refuses it before the limit
    // is consulted, with no message of its own; refused before allocating either way.
    if constexpr (sizeof(size_t) == 8)
        CHECK(cc::format("{}", refused.error()).contains("limit"));

    // The ceiling is a knob, and a tiny one refuses even an honest file.
    auto const honest = babel::test::fbx_binary_scene();
    CHECK(bf::read(cc::span<byte const>(honest), {.memory_limit = 1024}).has_error());
    CHECK(bf::read(cc::span<byte const>(honest), {.memory_limit = i64(256) << 20}).has_value());

    // 1 halves to 0, which ufbx would read as unlimited; the reader holds it at 1 byte instead.
    // The allocation count halves to 0 there too, and is held at 1, so only the memory clamp's own message pins it.
    // That second clamp decides no read on its own: under 64 bytes, where it applies, the memory clamp refuses first.
    auto const tiny = bf::read(cc::span<byte const>(honest), {.memory_limit = 1});
    REQUIRE(tiny.has_error());
    CHECK(cc::format("{}", tiny.error()).contains("Memory limit exceeded"));
}

TEST("fbx - a property template is held once, shared by every material inheriting it")
{
    // One template with a 64 KiB string, inherited by 200 materials that set nothing themselves.
    // Copied into each, it would be 12.5 MiB from a file of 80 KB.
    constexpr auto material_count = 200;
    auto const big = cc::string::create_filled(64 << 10, 'x');
    auto text = cc::string("; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXVersion: 7400\n}\n");
    text += cc::format("Definitions:  {{\n\tObjectType: \"Material\" {{\n\t\tPropertyTemplate: \"FbxSurfacePhong\" {{\n"
                       "\t\t\tProperties70:  {{\n\t\t\t\tP: \"Big\", \"KString\", \"\", \"\", \"{}\"\n"
                       "\t\t\t\tP: \"Shininess\", \"double\", \"Number\", \"\",5\n\t\t\t}}\n\t\t}}\n\t}}\n}}\n",
                       big);
    text += "Objects:  {\n";
    for (auto i = 0; i < material_count; ++i)
        text += cc::format(
            "\tMaterial: {}, \"Material::m{}\", \"\" {{\n\t\tShadingModel: \"phong\"\n{}\t}}\n", 1000 + i, i,
            i == 0 ? "\t\tProperties70:  {\n\t\t\tP: \"Shininess\", \"double\", \"Number\", \"\",20\n\t\t}\n" : "");
    text += "}\n";

    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(text)));
    REQUIRE(doc.materials.size() == material_count);

    // Every material points at the one copy.
    auto const* const shared = doc.materials[0].defaults.data();
    REQUIRE(shared != nullptr);
    auto all_shared = true;
    for (auto const& m : doc.materials)
        all_shared = all_shared && m.defaults.data() == shared;
    CHECK(all_shared);

    // The lookup still reports what the FBX SDK would: the material's own value first, then the template's.
    auto const* const inherited = doc.materials[1].find_property("Big");
    REQUIRE(inherited != nullptr);
    CHECK(inherited->text.size() == big.size());
    CHECK(doc.materials[1].properties.empty());
    CHECK(near(doc.materials[0].find_property("Shininess")->value[0], 20.0f));
    CHECK(near(doc.materials[1].find_property("Shininess")->value[0], 5.0f));
}

TEST("fbx - copying the meshes is budgeted against the memory limit before any of it is made")
{
    // 30,000 corners and 300 uv layers, each one value for the whole mesh.
    // ufbx stores every layer's indices as one shared buffer, babel's copy one array each: 36 MB from a file of 0.2 MB.
    constexpr auto triangle_count = 10000;
    constexpr auto layer_count = 300;
    auto corners = cc::string();
    for (auto i = 0; i < triangle_count; ++i)
        corners += i == 0 ? "0,1,-3" : ",0,1,-3";
    auto text = cc::string("; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXVersion: 7400\n}\nObjects:  {\n");
    text
        += cc::format("\tGeometry: 100, \"Geometry::m\", \"Mesh\" {{\n\t\tVertices: *9 {{\n\t\t\ta: 0,0,0,1,0,0,0,1,0\n"
                      "\t\t}}\n\t\tPolygonVertexIndex: *{} {{\n\t\t\ta: {}\n\t\t}}\n",
                      triangle_count * 3, corners);
    for (auto i = 0; i < layer_count; ++i)
        text += cc::format(
            "\t\tLayerElementUV: {} {{\n\t\t\tName: \"uv{}\"\n\t\t\tMappingInformationType: \"AllSame\"\n"
            "\t\t\tReferenceInformationType: \"Direct\"\n\t\t\tUV: *2 {{\n\t\t\t\ta: 0,0\n\t\t\t}}\n\t\t}}\n",
            i, i);
    text += "\t}\n\tModel: 200, \"Model::m\", \"Mesh\" {\n\t}\n}\nConnections:  {\n\tC: \"OO\",200,0\n\tC: "
            "\"OO\",100,200\n}\n";

    // 32 MB leaves ufbx's parse 16 MB, which it fits in; the copy does not fit the 32.
    auto const refused = bf::read(cc::string_view(text), {.memory_limit = i64(32) << 20});
    REQUIRE(refused.has_error());
    CHECK(cc::format("{}", refused.error()).contains("copying the meshes"));

    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(text), {.memory_limit = i64(64) << 20}));
    REQUIRE(doc.meshes.size() == 1);
    CHECK(doc.meshes[0].uv_sets.size() == layer_count);
}

TEST("fbx - axes that do not name X, Y and Z once each fall back to FBX's default, with an issue")
{
    // Up and right both X.
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene({.up_axis = 0}))));
    CHECK(doc.settings.right == bf::axis::positive_x);
    CHECK(doc.settings.up == bf::axis::positive_y);
    CHECK(doc.settings.front == bf::axis::positive_z);
    CHECK(doc.has_issue_of(bf::issue_kind::malformed));
}

TEST("fbx - a unit outside a nanometer to a million kilometers falls back to a centimeter, with an issue")
{
    auto const nan = cc::bit_cast<f64>(0x7FF8000000000000ull);
    auto const infinity = cc::bit_cast<f64>(0x7FF0000000000000ull);
    for (auto const factor : {0.0, -5.0, 1e-300, 1e35, nan, infinity})
    {
        auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(babel::test::fbx_scene({.unit_scale_factor = factor}))));
        CHECK(near(f32(doc.settings.unit_meters), 0.01f));
        CHECK(doc.has_issue_of(bf::issue_kind::malformed));
    }
}

TEST("fbx - a polygon past the corner cap is dropped rather than ear-clipped")
{
    // 5000 corners: an ear clip of it is quadratic, and the cap refuses it before ufbx starts.
    auto vertices = cc::string();
    auto corners = cc::string();
    constexpr auto count = 5000;
    for (auto i = 0; i < count; ++i)
    {
        // Points on a parabola, which make a convex polygon.
        vertices += cc::format("{}{},{},0", i == 0 ? "" : ",", i, i * i);
        corners += cc::format("{}{}", i == 0 ? "" : ",", i + 1 == count ? -count : i);
    }
    auto text = cc::string("; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXVersion: 7400\n}\nObjects:  {\n");
    text += cc::format("\tGeometry: 100, \"Geometry::disc\", \"Mesh\" {{\n\t\tVertices: *{} {{\n\t\t\ta: {}\n\t\t}}\n",
                       count * 3, vertices);
    text += cc::format("\t\tPolygonVertexIndex: *{} {{\n\t\t\ta: {}\n\t\t}}\n\t}}\n", count, corners);
    text += "\tModel: 200, \"Model::disc\", \"Mesh\" {\n\t}\n}\nConnections:  {\n\tC: \"OO\",200,0\n\tC: "
            "\"OO\",100,200\n}\n";

    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(text), {.triangulate = true}));
    REQUIRE(doc.meshes.size() == 1);
    CHECK(doc.meshes[0].faces.empty());
    CHECK(cc::string_view(doc.issue_report()).contains("4096"));

    // Untriangulated, the polygon is kept as the file wrote it.
    auto const raw = REQUIRED_VALUE(bf::read(cc::string_view(text)));
    REQUIRE(raw.meshes[0].faces.size() == 1);
    CHECK(raw.meshes[0].faces[0].corner_count == count);
}

TEST("fbx - a geometric transform moves the mesh and not the node's children")
{
    auto const text
        = babel::test::fbx_scene_with("\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",2,2,2\n",
                                      "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",2,2,2\n"
                                      "\t\t\tP: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",0,0,7\n");
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(text)));

    auto const* const parent = node_named(doc, "parent");
    auto const* const child = node_named(doc, "child");
    REQUIRE(parent != nullptr);
    REQUIRE(child != nullptr);
    CHECK(parent->has_geometry_transform);
    CHECK(near(parent->geometry.translation[2], 7.0f));

    // The mesh moves by 7, doubled by the node's scale; the node itself and its child stay where they were.
    CHECK(near(parent->geometry_to_world.transform(tg::pos3f::zero), tg::pos3f(10, 0, 14)));
    CHECK(near(parent->node_to_world.transform(tg::pos3f::zero), tg::pos3f(10, 0, 0)));
    CHECK(near(child->node_to_world.transform(tg::pos3f::zero), tg::pos3f(10, 10, 0)));
}

TEST("fbx - a node ignoring its parent's scale says so, and its world transform honors it")
{
    auto const text
        = babel::test::fbx_scene_with("\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,5,0\n",
                                      "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,5,0\n"
                                      "\t\t\tP: \"InheritType\", \"enum\", \"\", \"\",2\n");
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(text)));

    auto const* const child = node_named(doc, "child");
    REQUIRE(child != nullptr);
    CHECK(child->inherit == bf::inherit_mode::ignore_parent_scale);

    // The parent scales by 2; this child does not, which a plain product of local transforms would get wrong.
    CHECK(near(child->node_to_world.transform(tg::vec3f(1, 0, 0)).length(), 1.0f));
}

TEST("fbx - textures embedding one blob share it rather than each holding a copy")
{
    auto const text = babel::test::fbx_scene_with("\tTexture: 500,", "\tVideo: 600, \"Video::wood\", \"Clip\" {\n"
                                                                     "\t\tType: \"Clip\"\n"
                                                                     "\t\tRelativeFilename: \"wood.png\"\n"
                                                                     "\t\tContent: , \"AAECAw==\"\n"
                                                                     "\t}\n"
                                                                     "\tTexture: 501, \"Texture::wood2\", \"\" {\n"
                                                                     "\t\tType: \"TextureVideoClip\"\n"
                                                                     "\t\tRelativeFilename: \"wood.png\"\n"
                                                                     "\t}\n"
                                                                     "\tTexture: 500,");
    auto patched = cc::string(text);
    REQUIRE(patched.replace_first("\tC: \"OO\",300,0\n", "\tC: \"OO\",300,0\n\tC: \"OO\",600,500\n\tC: "
                                                         "\"OO\",600,501\n"));
    auto const doc = REQUIRED_VALUE(bf::read(cc::string_view(patched)));

    REQUIRE(doc.textures.size() == 2);
    REQUIRE(doc.textures[0].content.size() == 4);
    REQUIRE(doc.textures[1].content.size() == 4);
    CHECK(u8(doc.textures[0].content.span()[3]) == 3);
    CHECK(doc.textures[0].content.data() == doc.textures[1].content.data());
}

// Spreading a read over a scheduler
// -------------------------------------------------------------------------------------------------

TEST("fbx - a read spread over a pool is the read on the calling thread, field for field")
{
    auto pool = cc::async_thread_pool(2);

    // ASCII, and binary with compressed arrays, which is where ufbx's own tasks do their work.
    auto const text = babel::test::fbx_scene();
    auto const binary = babel::test::fbx_binary_scene();
    for (auto const triangulate : {false, true})
    {
        auto const plain = REQUIRED_VALUE(bf::read(cc::string_view(text), {.triangulate = triangulate}));
        auto const spread
            = REQUIRED_VALUE(bf::read(cc::string_view(text), {.triangulate = triangulate, .scheduler = &pool}));
        CHECK(first_difference(plain, spread).empty()).dump("difference", first_difference(plain, spread));

        auto const plain_binary = REQUIRED_VALUE(bf::read(cc::span<byte const>(binary), {.triangulate = triangulate}));
        auto const spread_binary
            = REQUIRED_VALUE(bf::read(cc::span<byte const>(binary), {.triangulate = triangulate, .scheduler = &pool}));
        CHECK(first_difference(plain_binary, spread_binary).empty())
            .dump("difference", first_difference(plain_binary, spread_binary));
    }
}

TEST("fbx - read_structure names every mesh and fills none, and reads everything else as read does")
{
    auto const text = babel::test::fbx_scene();
    auto const full = REQUIRED_VALUE(bf::read(cc::string_view(text)));
    auto const structure = REQUIRED_VALUE(bf::read_structure(cc::span<byte const>(cc::string_view(text).as_bytes())));

    REQUIRE(structure.meshes.size() == full.meshes.size());
    for (auto i = isize(0); i < full.meshes.size(); ++i)
    {
        CHECK(structure.meshes[i].name == full.meshes[i].name);
        CHECK(structure.meshes[i].faces.empty());
        CHECK(!structure.meshes[i].positions.exists());
        CHECK(!full.meshes[i].faces.empty());
    }

    // The same indices for everything a caller fetches or places before the geometry arrives.
    REQUIRE(structure.nodes.size() == full.nodes.size());
    for (auto i = isize(0); i < full.nodes.size(); ++i)
    {
        CHECK(structure.nodes[i].name == full.nodes[i].name);
        CHECK(structure.nodes[i].mesh == full.nodes[i].mesh);
        CHECK(structure.nodes[i].local.translation == full.nodes[i].local.translation);
        CHECK(structure.materials_of(structure.nodes[i]).size() == full.materials_of(full.nodes[i]).size());
    }
    REQUIRE(structure.materials.size() == full.materials.size());
    REQUIRE(structure.textures.size() == full.textures.size());
    CHECK(structure.textures[0].relative_filename == full.textures[0].relative_filename);
    CHECK(structure.materials[0].find_texture("DiffuseColor") == full.materials[0].find_texture("DiffuseColor"));
}

ASYNC_TEST("fbx - read_async resolves the structure and the whole scene, each as its synchronous call would")
{
    auto const text = babel::test::fbx_scene();
    auto const bytes = cc::string_view(text).as_bytes();

    // No scheduler named, so both stages run on the installed compute scheduler.
    auto const loading = bf::read_async(pinned_copy(bytes), {.triangulate = true});
    auto const structure = co_await loading.structure;
    auto const complete = co_await loading.complete;

    auto const expected_structure = REQUIRED_VALUE(bf::read_structure(bytes, {.triangulate = true}));
    auto const expected_complete = REQUIRED_VALUE(bf::read(bytes, {.triangulate = true}));
    CHECK(first_difference(structure, expected_structure).empty())
        .dump("difference", first_difference(structure, expected_structure));
    CHECK(first_difference(complete, expected_complete).empty())
        .dump("difference", first_difference(complete, expected_complete));
}

ASYNC_TEST("fbx - read_async on bytes that are not FBX fails both stages")
{
    auto const not_fbx = cc::string_view("this is not an FBX file at all, in either encoding");

    auto const loading = bf::read_async(pinned_copy(not_fbx.as_bytes()));
    co_await cc::async_settled(loading.structure);
    co_await cc::async_settled(loading.complete);
    CHECK(loading.structure->has_error());
    CHECK(loading.complete->has_error());
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
    auto const bytes = REQUIRED_VALUE(stream.read_all());

    auto const start = cc::current_time_steady_secs();
    auto const doc = REQUIRED_VALUE(bf::read(cc::span<byte const>(bytes), {.triangulate = true}));
    auto const plain_secs = cc::current_time_steady_secs() - start;

    CHECK(doc.meshes.size() == 1296);
    CHECK(doc.materials.size() == 132);
    CHECK(doc.lights.size() == 1);
    CHECK(near(f32(doc.settings.unit_meters), 1.0f));
    CHECK(doc.settings.up == bf::axis::positive_y);

    // The same read spread over a pool, which is what a scene this size is for; the timings are logged, not judged.
    auto pool = cc::async_thread_pool();
    auto const spread_start = cc::current_time_steady_secs();
    auto const spread = REQUIRED_VALUE(bf::read(cc::span<byte const>(bytes), {.triangulate = true, .scheduler = &pool}));
    auto const spread_secs = cc::current_time_steady_secs() - spread_start;
    CHECK(first_difference(doc, spread).empty()).dump("difference", first_difference(doc, spread));

    auto const structure_start = cc::current_time_steady_secs();
    auto const structure = REQUIRED_VALUE(bf::read_structure(cc::span<byte const>(bytes), {.scheduler = &pool}));
    auto const structure_secs = cc::current_time_steady_secs() - structure_start;
    CHECK(structure.meshes.size() == doc.meshes.size());
    CHECK(structure.textures.size() == doc.textures.size());

    CC_LOG_INFO("fbx: Bistro exterior read in {} s, {} s over {} workers; its structure alone in {} s", plain_secs,
                spread_secs, pool.worker_count(), structure_secs);
}
