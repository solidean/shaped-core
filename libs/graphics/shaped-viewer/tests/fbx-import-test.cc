#include <babel-serializer/geometry/fbx.hh>
#include <babel-serializer/image/image.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <geometry/fbx_fixtures.hh>
#include <nexus/test.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/scene/light.hh>

using namespace cc::primitive_defines;

// CPU-only tests for the FBX importer, over the scene fbx_fixtures.hh describes.
// No device and no filesystem: the bytes are in memory, and a texture comes from a resolver the test installs.

namespace
{
[[nodiscard]] sv::material_library make_library()
{
    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);
    return lib;
}

[[nodiscard]] cc::pinned_data<byte const> pin(cc::string_view text)
{
    return cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(text.as_bytes()));
}

[[nodiscard]] bool near(f32 a, f32 b)
{
    auto const d = a - b;
    return d < 1e-4f && d > -1e-4f;
}

[[nodiscard]] bool near(tg::pos3f a, tg::pos3f b)
{
    return near(a[0], b[0]) && near(a[1], b[1]) && near(a[2], b[2]);
}

/// Where a mesh's first position lands in the world.
[[nodiscard]] tg::pos3f placed_origin_of(sv::mesh const& m)
{
    return m.transform.transform(m.geometry.positions.span()[0]);
}

/// The constant a material binds to `name`, read as T; a default T when it binds none.
template <class T>
[[nodiscard]] T constant_of(sv::material const& m, cc::string_view name)
{
    auto value = T{};
    auto const* const b = m.find(name);
    if (b != nullptr && b->constant.size() == isize(sizeof(T)))
        cc::memcpy(&value, b->constant.data(), sizeof(T));
    return value;
}

[[nodiscard]] sv::mesh_texture const* texture_named(sv::mesh const& m, cc::string_view name)
{
    for (auto const& t : m.textures)
        if (t.name == name)
            return &t;
    return nullptr;
}

/// A 2x2 RGBA PNG, opaque unless `with_alpha`.
[[nodiscard]] cc::vector<byte> png_2x2(bool with_alpha)
{
    auto pixels = cc::vector<byte>::create_filled(16, byte(200));
    if (with_alpha)
        pixels[3] = byte(0);
    auto encoded = babel::image::encode(
        {.width = 2, .height = 2, .channels = 4, .comp = babel::image::component::u8, .pixels = cc::move(pixels)},
        babel::image::format::png);
    return cc::move(encoded.value());
}
} // namespace

TEST("sv::asset_loader - FBX is recognized by its extension")
{
    CHECK(sv::asset_format_of_uri("bistro.fbx").value() == sv::asset_format::fbx);
    CHECK(sv::asset_format_of_uri("BISTRO.FBX").value() == sv::asset_format::fbx);
}

TEST("sv::asset_loader - an FBX import converts a Z-up meter scene into sv's +Y up")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});

    // 3ds Max's convention: Z up, -Y front.
    auto const text = babel::test::fbx_scene({.up_axis = 2, .front_axis = 1, .front_sign = -1, .unit_scale_factor = 100});
    auto const asset = REQUIRED_VALUE(loader.load(pin(text), sv::asset_format::fbx, "max.fbx"));

    auto const* const parent = asset.find_mesh("parent.0");
    REQUIRE(parent != nullptr);

    // The quad's (0,0,0) under "parent" (at 10,0,0) is the file's (10,0,0): X stays X.
    CHECK(near(placed_origin_of(*parent), tg::pos3f(10, 0, 0)));

    // The file's +Z is sv's +Y, and the file's -Y (its front) is sv's +Z.
    auto const up = parent->transform.transform(tg::pos3f(0, 0, 1));
    CHECK(near(up, tg::pos3f(10, 2, 0))); // scaled by the node's 2
    auto const front = parent->transform.transform(tg::pos3f(0, -1, 0));
    CHECK(near(front, tg::pos3f(10, 0, 2)));
}

TEST("sv::asset_loader - an FBX import scales a centimeter scene to meters")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});

    auto const text = babel::test::fbx_scene({.unit_scale_factor = 1});
    auto const asset = REQUIRED_VALUE(loader.load(pin(text), sv::asset_format::fbx, "cm.fbx"));

    // "child" sits at the parent's (10,0,0) plus its own (0,5,0) doubled by the parent's scale — 10 and 10 centimeters.
    auto const* const child = asset.find_mesh("child.0");
    REQUIRE(child != nullptr);
    CHECK(near(placed_origin_of(*child), tg::pos3f(0.1f, 0.1f, 0)));

    auto const bounds = REQUIRED_VALUE(asset.bounds());
    CHECK(bounds.max[0] < 1.0f); // nothing is left at centimeter scale
}

TEST("sv::asset_loader - an instanced FBX mesh splits per material slot and shares its geometry")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});
    auto const asset = REQUIRED_VALUE(loader.load(pin(babel::test::fbx_scene()), sv::asset_format::fbx, "inst.fbx"));

    // Two nodes times two slots.
    REQUIRE(asset.meshes.size() == 4);
    auto const* const parent_quad = asset.find_mesh("parent.0");
    auto const* const parent_tri = asset.find_mesh("parent.1");
    auto const* const child_quad = asset.find_mesh("child.0");
    auto const* const child_tri = asset.find_mesh("child.1");
    REQUIRE(parent_quad != nullptr);
    REQUIRE(parent_tri != nullptr);
    REQUIRE(child_quad != nullptr);
    REQUIRE(child_tri != nullptr);

    // The quad is two triangles over four welded vertices; the triangle one over three.
    CHECK(parent_quad->geometry.indices.size() == 6);
    CHECK(parent_quad->geometry.positions.size() == 4);
    CHECK(parent_tri->geometry.indices.size() == 3);

    // Instancing: the same bytes under two transforms, which content hashing turns into one upload.
    CHECK(parent_quad->geometry.hash == child_quad->geometry.hash);
    CHECK(parent_quad->transform != child_quad->transform);

    // FBX connects materials to the node, so the same slot draws red on one instance and glow on the other.
    CHECK(parent_quad->material == asset.material("red"));
    CHECK(child_quad->material == asset.material("glow"));
    CHECK(parent_tri->material == asset.material("glow"));
    CHECK(child_tri->material == asset.material("red"));

    // The hierarchy came across too, parents first.
    REQUIRE(asset.nodes.size() == 4);
    for (auto i = isize(1); i < asset.nodes.size(); ++i)
        CHECK(asset.nodes[i].parent < i32(i));
}

TEST("sv::asset_loader - an FBX mesh's uvs are flipped to top-row-first and its normals become frames")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});
    auto const asset = REQUIRED_VALUE(loader.load(pin(babel::test::fbx_scene()), sv::asset_format::fbx, "uv.fbx"));

    auto const* const quad = asset.find_mesh("parent.0");
    REQUIRE(quad != nullptr);

    auto has_uv = false;
    auto has_frame = false;
    for (auto const& a : quad->attributes)
    {
        has_frame = has_frame || a.name == "tangent_frame";
        if (a.name != "uv")
            continue;
        has_uv = true;

        // The first corner is the file's uv (0, 0), which is the BOTTOM of an image; sv samples top row first.
        auto first = tg::vec2f::zero;
        REQUIRE(a.data.size() >= isize(sizeof(first)));
        cc::memcpy(&first, a.data.data(), sizeof(first));
        CHECK(near(first[1], 1.0f));
    }
    CHECK(has_uv);
    CHECK(has_frame);
}

TEST("sv::asset_loader - FBX Phong materials map onto OpenPBR")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});
    auto const asset = REQUIRED_VALUE(loader.load(pin(babel::test::fbx_scene()), sv::asset_format::fbx, "mat.fbx"));

    auto const& red = lib.get(asset.material("red"));
    CHECK(red.name == "mat.fbx/red");
    CHECK(near(constant_of<tg::vec3f>(red, "base_color")[0], 1.0f));
    CHECK(near(constant_of<f32>(red, "base_metalness"), 0.0f));

    // Shininess 20 as a Beckmann alpha of sqrt(2 / 22), and roughness the square root of that.
    CHECK(near(constant_of<f32>(red, "specular_roughness"), 0.5491f));

    // EmissiveColor at a luminance of EmissiveFactor.
    auto const& glow = lib.get(asset.material("glow"));
    CHECK(near(constant_of<tg::vec3f>(glow, "emission_color")[1], 1.0f));
    CHECK(near(constant_of<f32>(glow, "emission_luminance"), 4.0f));
    CHECK(red.find("emission_luminance") == nullptr);
}

TEST("sv::asset_loader - FBX textures resolve against the document, backslashes and all")
{
    auto lib = make_library();
    auto requested = cc::vector<cc::string>();
    auto const loader = sv::asset_loader({.materials = &lib,
                                          .resolve = [&](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
                                          {
                                              requested.push_back(cc::string(uri));
                                              if (uri == "assets/textures/wood.png")
                                                  return cc::pinned_data<byte const>(
                                                      cc::pinned_data<byte>::create_copy_of(png_2x2(true)));
                                              return cc::error("not here");
                                          }});

    auto const asset = REQUIRED_VALUE(
        loader.load(pin(babel::test::fbx_scene()), sv::asset_format::fbx, "assets/scene.fbx", "assets/scene.fbx"));

    // `textures\wood.png`, written on Windows, joined against the FBX's own directory with portable separators.
    REQUIRE(!requested.empty());
    CHECK(requested[0] == "assets/textures/wood.png");

    auto const* const quad = asset.find_mesh("parent.0"); // drawn by "red", which carries the texture
    REQUIRE(quad != nullptr);
    auto const* const base = texture_named(*quad, "base_color");
    REQUIRE(base != nullptr);
    CHECK(base->source.texture.format == sg::pixel_format::rgba8_unorm_srgb);

    // A texel with alpha below one makes the base color a cutout: opacity from its alpha, stepped at one half.
    auto const* const opacity = texture_named(*quad, "opacity");
    REQUIRE(opacity != nullptr);
    CHECK(opacity->source.swizzle == sv::channel_swizzle::of_channel(sv::texture_channel::a));
    CHECK(near(constant_of<f32>(lib.get(asset.material("red")), "alpha_cutoff"), 0.5f));

    // The glow instance of the same geometry carries no texture of red's.
    CHECK(texture_named(*asset.find_mesh("child.0"), "base_color") == nullptr);
}

TEST("sv::asset_loader - an FBX texture that cannot be fetched is an issue, not a failure")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib,
                                          .resolve = [](cc::string_view) -> cc::result<cc::pinned_data<byte const>>
                                          { return cc::error("offline"); }});

    auto const asset = REQUIRED_VALUE(loader.load(pin(babel::test::fbx_scene()), sv::asset_format::fbx, "x.fbx"));
    CHECK(asset.meshes.size() == 4);
    CHECK(texture_named(*asset.find_mesh("parent.0"), "base_color") == nullptr);

    auto mentioned = false;
    for (auto const& i : asset.issues)
        mentioned = mentioned || cc::string_view(i).contains("wood");
    CHECK(mentioned);
}

TEST("sv::asset_loader - an FBX point light is placed, converted and kept in candela")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});

    // Z up and meters: the lamp's (0, 0, 3) is sv's (0, 3, 0).
    auto const text = babel::test::fbx_scene({.up_axis = 2, .front_axis = 1, .front_sign = -1, .unit_scale_factor = 100});
    auto const asset = REQUIRED_VALUE(loader.load(pin(text), sv::asset_format::fbx, "lamp.fbx"));

    REQUIRE(asset.lights.size() == 1);
    auto const& l = asset.lights[0];
    CHECK(l.id == "lamp");
    CHECK(l.light.emission.unit == sv::light_unit::candela);
    CHECK(near(l.light.emission.intensity, 2.5f));
    CHECK(near(l.light.emission.color[1], 0.5f));
    CHECK(near(l.light.placement.transform(tg::pos3f::zero), tg::pos3f(0, 3, 0)));

    // Quadratic in the file, which is what sv's physical falloff is, so nothing was reported about it.
    for (auto const& i : asset.issues)
        CHECK(!cc::string_view(i).contains("decay"));
}

TEST("sv::asset_loader - the ORCA convention reads SpecularColor as packed occlusion, roughness and metalness")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader(
        {.materials = &lib,
         .resolve = [](cc::string_view) -> cc::result<cc::pinned_data<byte const>>
         { return cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(png_2x2(false))); },
         .fbx_materials = sv::fbx_material_convention::orca_metal_rough});

    // The fixture's texture moved onto SpecularColor and NormalMap, which is how an ORCA material connects them.
    auto text = babel::test::fbx_scene();
    auto const connection = cc::string_view("C: \"OP\",500,400, \"DiffuseColor\"");
    auto const at = text.find(connection);
    REQUIRE(at >= 0);
    auto patched = cc::string(text.subview({.offset = 0, .size = at}));
    patched += "C: \"OP\",500,400, \"SpecularColor\"\n\tC: \"OP\",500,400, \"NormalMap\"";
    patched += text.subview({.offset = at + connection.size(), .size = text.size() - at - connection.size()});

    auto const asset = REQUIRED_VALUE(loader.load(pin(patched), sv::asset_format::fbx, "orca.fbx"));
    auto const* const quad = asset.find_mesh("parent.0");
    REQUIRE(quad != nullptr);

    auto const* const roughness = texture_named(*quad, "specular_roughness");
    auto const* const metalness = texture_named(*quad, "base_metalness");
    auto const* const occlusion = texture_named(*quad, "occlusion");
    REQUIRE(roughness != nullptr);
    REQUIRE(metalness != nullptr);
    REQUIRE(occlusion != nullptr);
    CHECK(roughness->source.swizzle == sv::channel_swizzle::of_channel(sv::texture_channel::g));
    CHECK(metalness->source.swizzle == sv::channel_swizzle::of_channel(sv::texture_channel::b));
    CHECK(occlusion->source.swizzle == sv::channel_swizzle::of_channel(sv::texture_channel::r));
    CHECK(roughness->source.texture.format == sg::pixel_format::rgba8_unorm); // data, not color

    // A DirectX normal map: green points down the image, so its y is negated on the way in.
    auto const* const normal = texture_named(*quad, "normal");
    REQUIRE(normal != nullptr);
    CHECK(normal->source.transform.scale[1] < 0.0f);

    // A normal map wants tangents along the uv layout, which FBX did not carry, so they were generated.
    auto has_frame = false;
    for (auto const& a : quad->attributes)
        has_frame = has_frame || a.name == "tangent_frame";
    CHECK(has_frame);

    // The constant is read the same way where no map overrides it: the fixture's SpecularColor is the FBX default.
    auto const& red = lib.get(asset.material("red"));
    CHECK(red.find("base_metalness") != nullptr);
}

TEST("sv::asset_loader - an FBX normal map decoded with a zero third channel has it rebuilt")
{
    // What babel hands back for a BC5 normal map: x and y, then 0 and 1 — a flat (0, 0) tilt here.
    auto pixels = cc::vector<byte>();
    for (auto i = 0; i < 4; ++i)
        for (auto const c : {128, 128, 0, 255})
            pixels.push_back(byte(c));
    auto const png = babel::image::encode(
        {.width = 2, .height = 2, .channels = 4, .comp = babel::image::component::u8, .pixels = cc::move(pixels)},
        babel::image::format::png);
    REQUIRE(png.has_value());

    auto lib = make_library();
    auto const loader
        = sv::asset_loader({.materials = &lib,
                            .resolve = [&](cc::string_view) -> cc::result<cc::pinned_data<byte const>>
                            { return cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(png.value())); }});

    auto text = babel::test::fbx_scene();
    auto const connection = cc::string_view("C: \"OP\",500,400, \"DiffuseColor\"");
    auto const at = text.find(connection);
    REQUIRE(at >= 0);
    auto patched = cc::string(text.subview({.offset = 0, .size = at}));
    patched += "C: \"OP\",500,400, \"NormalMap\"";
    patched += text.subview({.offset = at + connection.size(), .size = text.size() - at - connection.size()});

    auto const asset = REQUIRED_VALUE(loader.load(pin(patched), sv::asset_format::fbx, "bc5.fbx"));
    auto const* const normal = texture_named(*asset.find_mesh("parent.0"), "normal");
    REQUIRE(normal != nullptr);

    // The third channel is the unit normal's z, which for an untilted texel is all the way up.
    auto const texels = normal->source.texture.pixels.span();
    REQUIRE(texels.size() >= 4);
    CHECK(u8(texels[2]) >= 254);
}

namespace
{
/// The shared scene with the texture's two file names replaced, as the FBX text spells them.
[[nodiscard]] cc::string scene_naming(cc::string_view relative, cc::string_view absolute)
{
    auto text = babel::test::fbx_scene();
    [[maybe_unused]] auto const replaced = text.replace_first(
        "\t\tFileName: \"C:/somewhere/textures/wood.png\"\n\t\tRelativeFilename: \"textures\\wood.png\"\n",
        cc::format("\t\tFileName: \"{}\"\n\t\tRelativeFilename: \"{}\"\n", absolute, relative));
    return text;
}

/// Every uri an import of `text` asked the resolver for, none of which resolves.
[[nodiscard]] cc::vector<cc::string> requests_of(cc::string_view text, cc::vector<cc::string>* issues = nullptr)
{
    auto lib = make_library();
    auto requested = cc::vector<cc::string>();
    auto const loader = sv::asset_loader({.materials = &lib,
                                          .resolve = [&](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
                                          {
                                              requested.push_back(cc::string(uri));
                                              return cc::error("not here");
                                          }});
    auto const asset = loader.load(pin(text), sv::asset_format::fbx, "assets/scene.fbx", "assets/scene.fbx");
    if (issues != nullptr && asset.has_value())
        *issues = asset.value().issues;
    return requested;
}

[[nodiscard]] bool mentions(cc::span<cc::string const> issues, cc::string_view what)
{
    for (auto const& i : issues)
        if (cc::string_view(i).contains(what))
            return true;
    return false;
}
} // namespace

TEST("sv::asset_loader - an FBX texture is asked for beside the document, then by its bare name, never absolutely")
{
    // The relative name first, then the bare file name; the exporting machine's absolute path never.
    auto const plain = requests_of(scene_naming("textures\\wood.png", "C:/somewhere/textures/wood.png"));
    REQUIRE(plain.size() == 2);
    CHECK(plain[0] == "assets/textures/wood.png");
    CHECK(plain[1] == "assets/wood.png");

    // A "relative" name rooted at /, or naming a drive, is asked for only by its bare name — a Windows-authored FBX's case.
    auto const rooted = requests_of(scene_naming("/elsewhere/wood.png", ""));
    REQUIRE(rooted.size() == 1);
    CHECK(rooted[0] == "assets/wood.png");
    auto const windows = requests_of(scene_naming("C:\\Users\\a\\tex.png", ""));
    REQUIRE(windows.size() == 1);
    CHECK(windows[0] == "assets/tex.png");

    // A ../ still climbs out of the document's directory, as it does for glTF.
    auto const parent = requests_of(scene_naming("../shared/wood.png", ""));
    REQUIRE(!parent.empty());
    CHECK(parent[0] == "assets/../shared/wood.png");
}

TEST("sv::asset_loader - an FBX texture on a network path is never fetched as one, and says so")
{
    // Only the bare name, beside the document, is asked for — never the share.
    auto issues = cc::vector<cc::string>();
    auto const requested = requests_of(scene_naming("\\\\server\\share\\wood.png", ""), &issues);
    REQUIRE(requested.size() == 1);
    CHECK(requested[0] == "assets/wood.png");
    CHECK(mentions(issues, "network path"));

    auto const forward = requests_of(scene_naming("//server/share/wood.png", ""), &issues);
    REQUIRE(forward.size() == 1);
    CHECK(forward[0] == "assets/wood.png");
}

TEST("sv::asset_loader - an FBX texture naming a Windows device is never asked for")
{
    auto issues = cc::vector<cc::string>();
    CHECK(requests_of(scene_naming("textures/CON.png", ""), &issues).empty());
    CHECK(mentions(issues, "device"));
    CHECK(requests_of(scene_naming("nul", "")).empty());
    CHECK(requests_of(scene_naming("Lpt9.tga", "")).empty());

    // A device in a directory component refuses the whole name; the bare name is still fine.
    auto const nested = requests_of(scene_naming("aux.dir/wood.png", ""));
    REQUIRE(nested.size() == 1);
    CHECK(nested[0] == "assets/wood.png");

    // Names that merely start like one are ordinary files.
    CHECK(!requests_of(scene_naming("console.png", "")).empty());
    CHECK(!requests_of(scene_naming("COM10.png", "")).empty());
}

TEST("sv::asset_loader - an ORCA packed map that does not decode leaves a neutral surface, not the constant")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib,
                                          .resolve = [](cc::string_view) -> cc::result<cc::pinned_data<byte const>>
                                          {
                                              // Bytes, but no image babel can decode.
                                              return cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(
                                                  cc::string_view("DDS not yet").as_bytes()));
                                          },
                                          .fbx_materials = sv::fbx_material_convention::orca_metal_rough});

    auto const text
        = babel::test::fbx_scene_with("C: \"OP\",500,400, \"DiffuseColor\"", "C: \"OP\",500,400, \"SpecularColor\"");
    auto const asset = REQUIRED_VALUE(loader.load(pin(text), sv::asset_format::fbx, "packed.fbx"));

    // "red" states no SpecularColor, so its constant is the 0.5 default — which would make it half metal.
    auto const& red = lib.get(asset.material("red"));
    CHECK(near(constant_of<f32>(red, "base_metalness"), 0.0f));
    CHECK(near(constant_of<f32>(red, "specular_roughness"), 0.5f));
    CHECK(mentions(asset.issues, "packed specular map"));
}

TEST("sv::asset_loader - a hidden FBX node's meshes stay out")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});

    auto const text
        = babel::test::fbx_scene_with("\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,5,0\n",
                                      "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,5,0\n"
                                      "\t\t\tP: \"Visibility\", \"Visibility\", \"\", \"A\",0\n");
    auto const asset = REQUIRED_VALUE(loader.load(pin(text), sv::asset_format::fbx, "hidden.fbx"));

    CHECK(asset.find_mesh("parent.0") != nullptr);
    CHECK(asset.find_mesh("child.0") == nullptr);
    CHECK(asset.find_mesh("child.1") == nullptr);
    CHECK(mentions(asset.issues, "hidden"));
}

namespace
{
/// The shared scene with one more property on node "parent", after its scaling.
[[nodiscard]] cc::string scene_with_parent_property(cc::string_view property, babel::test::fbx_settings s = {})
{
    auto const scaling = cc::string_view("\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",2,2,2\n");
    return babel::test::fbx_scene_with(scaling, cc::format("{}\t\t\t{}\n", scaling, property), s);
}
} // namespace

TEST("sv::asset_loader - a hidden FBX node hides its whole subtree, lights included")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});

    // "parent" hidden, "child" under it visible on its own: FBX's VisibilityInheritance hides both.
    auto const group = REQUIRED_VALUE(loader.load(pin(scene_with_parent_property("P: \"Visibility\", \"Visibility\", "
                                                                                 "\"\", \"A\",0")),
                                                  sv::asset_format::fbx, "group.fbx"));
    CHECK(group.meshes.empty());
    CHECK(group.lights.size() == 1); // the lamp hangs off the root
    CHECK(mentions(group.issues, "2 node(s) placing a mesh are hidden"));

    // A hidden light node is left out like a hidden mesh.
    auto const lamp
        = babel::test::fbx_scene_with("\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,3\n",
                                      "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,3\n"
                                      "\t\t\tP: \"Visibility\", \"Visibility\", \"\", \"A\",0\n");
    auto const dark = REQUIRED_VALUE(loader.load(pin(lamp), sv::asset_format::fbx, "dark.fbx"));
    CHECK(dark.lights.empty());
    CHECK(dark.meshes.size() == 4);
    CHECK(mentions(dark.issues, "light(s) are hidden"));
}

TEST("sv::asset_loader - an FBX hole face imports no triangles")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});

    // Read without triangulation, which is what keeps hole faces; the quad, slot 0, is marked as one by hand.
    auto doc = REQUIRED_VALUE(babel::fbx::read(cc::string_view(babel::test::fbx_scene())));
    REQUIRE(doc.meshes.size() == 1);
    REQUIRE(doc.meshes[0].faces.size() == 2);

    auto const whole = REQUIRED_VALUE(loader.load(doc, "whole.fbx"));
    CHECK(whole.meshes.size() == 4);
    CHECK(mentions(whole.issues, "fanned")); // the quad, untriangulated

    doc.meshes[0].face_holes = {true, false};
    auto const holed = REQUIRED_VALUE(loader.load(doc, "holed.fbx"));

    // Only the triangle is left, so each instance is one part named after its node.
    REQUIRE(holed.meshes.size() == 2);
    for (auto const& m : holed.meshes)
        CHECK(m.geometry.indices.size() == 3);
    CHECK(holed.find_mesh("parent") != nullptr);
    CHECK(holed.find_mesh("child") != nullptr);
    CHECK(!mentions(holed.issues, "fanned"));
}

TEST("sv::asset_loader - a left-handed FBX scene is mirrored into sv's, with an issue")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});

    // Y up and +X right with a -Z front: X cross Y is +Z, so the file is left-handed.
    auto const asset = REQUIRED_VALUE(loader.load(
        pin(babel::test::fbx_scene({.front_sign = -1, .unit_scale_factor = 100})), sv::asset_format::fbx, "lh.fbx"));

    auto const* const quad = asset.find_mesh("parent.0");
    REQUIRE(quad != nullptr);
    // Right and up stay; the file's +Z, its BACK, lands on sv's -Z — a mirror no rotation reaches.
    CHECK(near(quad->transform.transform(tg::pos3f(1, 0, 0)), tg::pos3f(12, 0, 0)));
    CHECK(near(quad->transform.transform(tg::pos3f(0, 1, 0)), tg::pos3f(10, 2, 0)));
    CHECK(near(quad->transform.transform(tg::pos3f(0, 0, 1)), tg::pos3f(10, 0, -2)));
    CHECK(mentions(asset.issues, "left-handed"));
}

TEST("sv::asset_loader - a two-channel FBX normal map is read as x and y with z rebuilt")
{
    // Gray plus alpha, as a decoder hands back a two-channel image: x 200, y 100.
    auto pixels = cc::vector<byte>();
    for (auto i = 0; i < 4; ++i)
    {
        pixels.push_back(byte(200));
        pixels.push_back(byte(100));
    }
    auto const png = babel::image::encode(
        {.width = 2, .height = 2, .channels = 2, .comp = babel::image::component::u8, .pixels = cc::move(pixels)},
        babel::image::format::png);
    REQUIRE(png.has_value());

    auto lib = make_library();
    auto const loader
        = sv::asset_loader({.materials = &lib,
                            .resolve = [&](cc::string_view) -> cc::result<cc::pinned_data<byte const>>
                            { return cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(png.value())); }});

    auto const text
        = babel::test::fbx_scene_with("C: \"OP\",500,400, \"DiffuseColor\"", "C: \"OP\",500,400, \"NormalMap\"");
    auto const asset = REQUIRED_VALUE(loader.load(pin(text), sv::asset_format::fbx, "rg.fbx"));
    auto const* const normal = texture_named(*asset.find_mesh("parent.0"), "normal");
    REQUIRE(normal != nullptr);

    auto const texels = normal->source.texture.pixels.span();
    REQUIRE(texels.size() >= 4);
    CHECK(u8(texels[0]) == 200);
    CHECK(u8(texels[1]) == 100); // y from the second channel, not the gray spread over rgb
    CHECK(u8(texels[3]) == 255);
    // z = sqrt(1 - x^2 - y^2) of the decoded (0.569, -0.216), stored back in [0,1]: 229.
    CHECK(u8(texels[2]) >= 227);
    CHECK(u8(texels[2]) <= 231);
}

namespace
{
/// Every uri an import of the shared scene asked for, with the texture's relative name `relative`, against `base`.
[[nodiscard]] cc::vector<cc::string> requests_for(cc::string_view relative,
                                                  cc::string_view base,
                                                  cc::vector<cc::string>* issues = nullptr)
{
    auto lib = make_library();
    auto requested = cc::vector<cc::string>();
    auto const loader = sv::asset_loader({.materials = &lib,
                                          .resolve = [&](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
                                          {
                                              requested.push_back(cc::string(uri));
                                              return cc::error("not here");
                                          }});
    auto const asset = loader.load(pin(scene_naming(relative, "")), sv::asset_format::fbx, "scene.fbx", base);
    if (issues != nullptr && asset.has_value())
        *issues = asset.value().issues;
    return requested;
}
} // namespace

TEST("sv::asset_loader - an FBX texture name is a file path: schemes, drives and escapes never reach the resolver as "
     "such")
{
    auto issues = cc::vector<cc::string>();

    // A scheme is not a file name, whichever one.
    // Only ever the bare file name beside the document, never the place the scheme names.
    auto const web = requests_for("https://example.com/wood.png", "assets/scene.fbx", &issues);
    REQUIRE(web.size() == 1);
    CHECK(web[0] == "assets/wood.png");
    CHECK(mentions(issues, "scheme"));
    auto const share = requests_for("file:////srv/share/wood.png", "assets/scene.fbx");
    REQUIRE(share.size() == 1);
    CHECK(share[0] == "assets/wood.png");

    // A drive-relative name is a drive too, bare file name and all.
    CHECK(requests_for("C:wood.png", "assets/scene.fbx").empty());

    // Escapes are escaped, so the resolver's decoding hands back the literal name rather than a network path.
    auto const escaped = requests_for("%5C%5Cattacker%5Cshare%5Cx.png", "");
    REQUIRE(!escaped.empty());
    CHECK(escaped[0] == "%255C%255Cattacker%255Cshare%255Cx.png");
    CHECK(sv::impl::percent_decode(escaped[0]) == "%5C%5Cattacker%5Cshare%5Cx.png");

    // And a real name holding a percent sign still opens the file it names.
    auto const percent = requests_for("100%.png", "assets/scene.fbx");
    REQUIRE(!percent.empty());
    CHECK(sv::impl::percent_decode(percent[0]) == "assets/100%.png");
}

TEST("sv::asset_loader - hostile FBX light values are held to what sv can mean, not asserted on")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});

    // A negative and an overflowing color channel, a negative intensity.
    auto const bad_color = babel::test::fbx_scene_with("P: \"Color\", \"Color\", \"\", \"A\",1,0.5,0.25",
                                                       "P: \"Color\", \"Color\", \"\", \"A\",-1,1e999,0.25");
    auto const dim = babel::test::fbx_scene_with("P: \"Intensity\", \"Number\", \"\", \"A\",250",
                                                 "P: \"Intensity\", \"Number\", \"\", \"A\",-5");
    for (auto const& text : {cc::string(bad_color), cc::string(dim)})
    {
        auto const asset = REQUIRED_VALUE(loader.load(pin(text), sv::asset_format::fbx, "hostile.fbx"));
        REQUIRE(asset.lights.size() == 1);
        auto const& e = asset.lights[0].light.emission;
        CHECK(e.intensity >= 0.0f);
        CHECK(e.color[0] >= 0.0f);
        CHECK(e.color[1] < 1e31f);
        CHECK(!asset.issues.empty());
    }

    // A directional light whose node scales its direction away to nothing is skipped, with an issue.
    auto const collapsed
        = babel::test::fbx_scene_with("\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,3\n",
                                      "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,3\n"
                                      "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",0,0,0\n");
    auto skipped_text = cc::string(collapsed);
    REQUIRE(skipped_text.replace_first("P: \"LightType\", \"enum\", \"\", \"\",0", "P: \"LightType\", \"enum\", \"\", "
                                                                                   "\"\",1"));
    auto const skipped = REQUIRED_VALUE(loader.load(pin(skipped_text), sv::asset_format::fbx, "collapsed.fbx"));
    CHECK(skipped.lights.empty());
    CHECK(mentions(skipped.issues, "direction"));
}

TEST("sv::asset_loader - FBX material values that are not finite and non-negative are clamped, with an issue")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});

    auto text = babel::test::fbx_scene_with("P: \"DiffuseColor\", \"Color\", \"\", \"A\",0,0,1",
                                            "P: \"DiffuseColor\", \"Color\", \"\", \"A\",-1,1e999,1");
    REQUIRE(text.replace_first("P: \"EmissiveFactor\", \"Number\", \"\", \"A\",4", "P: \"EmissiveFactor\", \"Number\", "
                                                                                   "\"\", \"A\",-4"));
    auto const asset = REQUIRED_VALUE(loader.load(pin(text), sv::asset_format::fbx, "clamped.fbx"));

    auto const& glow = lib.get(asset.material("glow"));
    auto const base = constant_of<tg::vec3f>(glow, "base_color");
    CHECK(base[0] == 0.0f);
    CHECK(base[1] <= 1e30f);
    CHECK(constant_of<f32>(glow, "emission_luminance") == 0.0f);
    CHECK(mentions(asset.issues, "clamped"));
}

TEST("sv::asset_loader - corners hanging off one control point weld in linear time")
{
    // Every triangle shares its three control points, but each corner carries its own uv, so nothing welds.
    // A weld scanning a control point's vertices would compare each new corner against all before it.
    constexpr auto triangles = 20000;
    auto corners = cc::string();
    auto uvs = cc::string();
    for (auto t = 0; t < triangles; ++t)
    {
        corners += cc::format("{}0,1,-3", t == 0 ? "" : ",");
        for (auto k = 0; k < 3; ++k)
            uvs += cc::format("{}{},{}", t == 0 && k == 0 ? "" : ",", t * 3 + k, k);
    }
    auto text = cc::string("; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXVersion: 7400\n}\nObjects:  {\n");
    text += "\tGeometry: 100, \"Geometry::fan\", \"Mesh\" {\n\t\tVertices: *9 {\n\t\t\ta: 0,0,0,1,0,0,0,1,0\n\t\t}\n";
    text += cc::format("\t\tPolygonVertexIndex: *{} {{\n\t\t\ta: {}\n\t\t}}\n", triangles * 3, corners);
    text
        += cc::format("\t\tLayerElementUV: 0 {{\n\t\t\tVersion: 101\n\t\t\tName: "
                      "\"map1\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
                      "\t\t\tReferenceInformationType: \"Direct\"\n\t\t\tUV: *{} {{\n\t\t\t\ta: {}\n\t\t\t}}\n\t\t}}\n",
                      triangles * 6, uvs);
    text += "\t\tLayer: 0 {\n\t\t\tVersion: 100\n\t\t\tLayerElement:  {\n\t\t\t\tType: "
            "\"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t}\n\t}\n";
    text += "\tModel: 200, \"Model::fan\", \"Mesh\" {\n\t}\n}\nConnections:  {\n\tC: \"OO\",200,0\n\tC: "
            "\"OO\",100,200\n}\n";

    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});
    auto const asset = REQUIRED_VALUE(loader.load(pin(text), sv::asset_format::fbx, "fan.fbx"));
    REQUIRE(asset.meshes.size() == 1);
    CHECK(asset.meshes[0].geometry.positions.size() == triangles * 3);
}

TEST("sv::asset_loader - NaN normals weld like any other value, rather than piling up one chain")
{
    // A binary FBX, since only it can carry an exact NaN: many triangles over three control points, every corner's normal NaN.
    // A compare of floats would never find two NaNs equal, and would walk one chain per corner.
    constexpr auto triangles = 20000;
    auto const object_name = [](cc::string_view name, cc::string_view type)
    {
        auto out = cc::string(name);
        out += cc::string_view("\0\x01", 2);
        out += type;
        return out;
    };
    auto const zero = 0.0;
    auto const nan = zero / zero;

    auto w = babel::test::fbx_binary_writer();
    w.begin("Objects");
    w.begin("Geometry");
    w.property(i64(100));
    w.property(cc::string_view(object_name("nan", "Geometry")));
    w.property(cc::string_view("Mesh"));
    auto const vertices = cc::vector<f64>{0, 0, 0, 1, 0, 0, 0, 1, 0};
    w.begin("Vertices");
    w.array(vertices);
    w.end();
    auto corners = cc::vector<i32>();
    for (auto t = 0; t < triangles; ++t)
    {
        corners.push_back(0);
        corners.push_back(1);
        corners.push_back(-3);
    }
    w.begin("PolygonVertexIndex");
    w.array(corners);
    w.end();
    w.begin("LayerElementNormal");
    w.property(i32(0));
    w.begin("MappingInformationType");
    w.property(cc::string_view("ByPolygonVertex"));
    w.end();
    w.begin("ReferenceInformationType");
    w.property(cc::string_view("Direct"));
    w.end();
    auto normals = cc::vector<f64>::create_filled(isize(triangles) * 9, nan);
    w.begin("Normals");
    w.array(normals);
    w.end();
    w.end();
    w.begin("Layer");
    w.property(i32(0));
    w.begin("LayerElement");
    w.begin("Type");
    w.property(cc::string_view("LayerElementNormal"));
    w.end();
    w.begin("TypedIndex");
    w.property(i32(0));
    w.end();
    w.end();
    w.end();
    w.end();
    w.begin("Model");
    w.property(i64(200));
    w.property(cc::string_view(object_name("nan", "Model")));
    w.property(cc::string_view("Mesh"));
    w.end();
    w.end();
    w.begin("Connections");
    for (auto const& link : {cc::vector<i64>{100, 200}, cc::vector<i64>{200, 0}})
    {
        w.begin("C");
        w.property(cc::string_view("OO"));
        w.property(link[0]);
        w.property(link[1]);
        w.end();
    }
    w.end();
    auto const bytes = w.finish();

    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_textures = false});
    auto const asset = REQUIRED_VALUE(loader.load(
        cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(bytes)), sv::asset_format::fbx, "nan.fbx"));
    REQUIRE(asset.meshes.size() == 1);
    CHECK(asset.meshes[0].geometry.positions.size() == 3);
    CHECK(asset.meshes[0].geometry.indices.size() == triangles * 3);
}

TEST("sv::asset_loader - the filesystem default reads regular files under the ceiling, and nothing else")
{
    auto const path = cc::temp_file_path("sv-fbx-resolver", ".bin");
    {
        auto file = REQUIRED_VALUE(cc::file_write_stream_adapter::create(path));
        auto stream = file.stream();
        auto const payload = cc::vector<byte>::create_filled(64, byte(7));
        REQUIRE(stream.write(payload).has_value());
        REQUIRE(stream.flush().has_value());
    }

    CHECK(sv::impl::resolve_uri_from_filesystem(path, 1024).has_value());
    auto const over = sv::impl::resolve_uri_from_filesystem(path, 16);
    REQUIRE(over.has_error());
    CHECK(cc::format("{}", over.error()).contains("bytes"));
    CHECK(cc::remove_file(path));

    // A directory is not a file, however a document names it.
    CHECK(sv::impl::resolve_uri_from_filesystem(cc::temp_directory_path()).has_error());

    // A device states one size and yields another; where there is none, there is nothing to prove.
    if (cc::file_read_stream_adapter::open("/dev/zero").has_error())
        SKIP("no /dev/zero on this platform");
    CHECK(sv::impl::resolve_uri_from_filesystem("/dev/zero").has_error());
}

// The real thing, when a developer has fetched it.
// Importing all of Bistro's exterior geometry takes seconds, so only the thorough run does it, and with no textures.
TEST("sv::asset_loader - Bistro exterior imports when present")
{
    if (!nx::is_thorough())
        SKIP("thorough only: imports a 114 MB scene");

    // Absent is a skip; present and failing to load is a failure, which is what this test is for.
    if (cc::file_read_stream_adapter::open(SV_TEST_BISTRO_DIR "/BistroExterior.fbx").has_error())
        SKIP("Bistro is not fetched; uv run extern/bistro/fetch-bistro.py installs it");

    auto lib = make_library();
    auto const loader = sv::asset_loader(
        {.materials = &lib, .import_textures = false, .fbx_materials = sv::fbx_material_convention::orca_metal_rough});
    auto const asset = REQUIRED_VALUE(loader.load(SV_TEST_BISTRO_DIR "/BistroExterior.fbx"));

    CHECK(asset.materials.size() >= 132);
    CHECK(asset.lights.size() == 1);
    CHECK(asset.meshes.size() >= 1296);

    // Welded: Bistro's normals are stored per polygon corner, so a weld that compared indices rather than values would leave three vertices per triangle.
    auto triangles = isize(0);
    auto vertices = isize(0);
    for (auto const& m : asset.meshes)
    {
        triangles += m.geometry.indices.size() / 3;
        vertices += m.geometry.positions.size();
    }
    CHECK(vertices < triangles * 3 / 2);

    // Meters already, Y up: the whole street fits inside a few hundred meters and stands on the ground.
    auto const bounds = REQUIRED_VALUE(asset.bounds());
    CHECK(bounds.max[1] - bounds.min[1] < 200.0f);
    CHECK(bounds.max[0] - bounds.min[0] < 1000.0f);
}
