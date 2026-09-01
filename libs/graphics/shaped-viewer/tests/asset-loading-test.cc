#include <babel-serializer/data/base64.hh>
#include <babel-serializer/geometry/gltf.hh>
#include <babel-serializer/geometry/obj.hh>
#include <babel-serializer/geometry/stl.hh>
#include <babel-serializer/image/image.hh>
#include <clean-core/common/utility.hh> // cc::memcpy, cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/material_type.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/pos_ops.hh>

using namespace cc::primitive_defines;

// CPU-only tests for the asset importer.
//
// No GPU and no filesystem: every document is built in memory, and the uri path runs through a resolver the test
// installs — which is the whole point of the loader holding no device and opening no file.

namespace
{
/// A library with the builtins, so an import has an `openpbr` type to mint into.
/// Its own rather than the process-wide one, so a test never depends on what another test acquired.
[[nodiscard]] sv::material_library make_library()
{
    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);
    return lib;
}

void push_le_u16(cc::vector<byte>& out, u16 value)
{
    out.push_back(byte(u8(value & 0xFFu)));
    out.push_back(byte(u8((value >> 8) & 0xFFu)));
}

void push_le_f32(cc::vector<byte>& out, f32 value)
{
    auto bits = u32(0);
    cc::memcpy(&bits, &value, sizeof(bits));
    for (auto k = 0; k < 4; ++k)
        out.push_back(byte(u8((bits >> (8 * k)) & 0xFFu)));
}

/// Three u16 indices, two padding bytes so the positions start 4-byte aligned, three vec3 positions, then three vec2
/// uvs — 68 bytes, with the uvs at offset 44.
[[nodiscard]] cc::vector<byte> triangle_bin()
{
    auto out = cc::vector<byte>();
    push_le_u16(out, 0);
    push_le_u16(out, 1);
    push_le_u16(out, 2);
    out.push_back(byte(0));
    out.push_back(byte(0));

    for (auto const v : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f})
        push_le_f32(out, v);
    for (auto const v : {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f})
        push_le_f32(out, v);
    return out;
}

/// A glTF whose one mesh carries `primitives`, with `extra` splicing in top-level members (materials, ...).
[[nodiscard]] cc::string triangle_gltf(cc::string_view primitives, cc::string_view extra)
{
    auto const bin = triangle_bin();
    auto json = cc::string();
    json += R"({"asset": {"version": "2.0"},
        "buffers": [{"uri": "data:application/octet-stream;base64,)";
    json += babel::base64::encode(bin);
    json += cc::format(R"(", "byteLength": {}}}],)", bin.size());
    json += R"(
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 6},
            {"buffer": 0, "byteOffset": 8, "byteLength": 36},
            {"buffer": 0, "byteOffset": 44, "byteLength": 24}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5123, "count": 3, "type": "SCALAR"},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3",
             "min": [0, 0, 0], "max": [1, 1, 0]},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"}
        ],)";
    json += extra;
    json += cc::format(R"(
        "meshes": [{{"name": "tri", "primitives": [{}]}}],
        "nodes": [{{"mesh": 0, "name": "root", "translation": [1, 2, 3]}}],
        "scenes": [{{"nodes": [0], "name": "main"}}],
        "scene": 0}})",
                       primitives);
    return json;
}

/// A quad under `red` and a triangle under `blue`, with uvs and one shared normal.
constexpr cc::string_view two_material_obj = R"obj(
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
usemtl red
f 1/1/1 2/2/1 3/3/1 4/4/1
usemtl blue
f 1/1/1 2/2/1 3/3/1
)obj";
} // namespace

TEST("sv - uri helpers do the joining and decoding babel deliberately does not")
{
    CHECK(sv::impl::extension_of("car.glb") == "glb");
    CHECK(sv::impl::extension_of("dir.v2/model.GLTF") == "gltf");
    // A fragment names a part of the file, not a different file.
    CHECK(sv::impl::extension_of("car.glb#mesh3") == "glb");
    CHECK(sv::impl::extension_of("noextension") == "");
    CHECK(sv::impl::extension_of("dir.v2/noextension") == "");

    CHECK(sv::impl::percent_decode("my%20model.obj") == "my model.obj");
    // A lone percent is a caller's byte, not an error.
    CHECK(sv::impl::percent_decode("100%") == "100%");

    // A relative reference joins against the directory its document came from.
    CHECK(sv::impl::join_uri("assets/car.gltf", "car.bin") == "assets/car.bin");
    CHECK(sv::impl::join_uri("car.gltf", "car.bin") == "car.bin");
    // Anything naming its own location is left alone.
    CHECK(sv::impl::join_uri("assets/car.gltf", "/abs/car.bin") == "/abs/car.bin");
    CHECK(sv::impl::join_uri("assets/car.gltf", "http://x/car.bin") == "http://x/car.bin");
    CHECK(sv::impl::join_uri("assets/car.gltf", "C:/abs/car.bin") == "C:/abs/car.bin");

    CHECK(sv::asset_format_of_uri("car.glb").value() == sv::asset_format::gltf);
    CHECK(sv::asset_format_of_uri("car.gltf").value() == sv::asset_format::gltf);
    CHECK(sv::asset_format_of_uri("car.obj").value() == sv::asset_format::obj);
    CHECK(!sv::asset_format_of_uri("car.fbx").has_value());
}

TEST("sv::asset_loader - an OBJ import triangulates, dedups and splits at usemtl")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib});

    auto const doc = babel::obj::read(two_material_obj);
    REQUIRE(doc.has_value());

    auto asset = loader.load(doc.value(), "quad.obj");
    REQUIRE(asset.has_value());
    auto& a = asset.value();

    // One mesh per usemtl run, named after the material the run named.
    REQUIRE(a.meshes.size() == 2);
    CHECK(a.meshes[0].name == "red");
    CHECK(a.meshes[1].name == "blue");

    // The quad is fanned into two triangles, and its four corners weld back to four vertices — six is what a fan
    // without dedup would leave behind.
    CHECK(a.meshes[0].geometry.triangle_count() == 2);
    CHECK(a.meshes[0].geometry.vertex_count() == 4);
    CHECK(a.meshes[1].geometry.triangle_count() == 1);
    CHECK(a.meshes[1].geometry.vertex_count() == 3);

    // uvs come across as the set every material's default `uv_attribute` looks for, and the normal as a tangent frame
    // rather than as a normal — which is what the renderer actually reads.
    auto const* const uv = [&]() -> sv::mesh_attribute const*
    {
        for (auto const& attr : a.meshes[0].attributes)
            if (attr.name == "uv")
                return &attr;
        return nullptr;
    }();
    REQUIRE(uv != nullptr);
    CHECK(uv->frequency == sv::attribute_frequency::per_vertex);
    CHECK(uv->element_count() == 4);

    auto has = [&](cc::string_view name)
    {
        for (auto const& attr : a.meshes[0].attributes)
            if (attr.name == name)
                return true;
        return false;
    };
    CHECK(has("tangent_frame"));
    CHECK(has("tangent_handedness"));

    // Each name is one material slot.
    REQUIRE(a.materials.size() == 2);
    CHECK(a.material("red") != sv::material_id::invalid);
    CHECK(a.material("nope") == sv::material_id::invalid);
    CHECK(a.meshes[0].material == a.material("red"));

    // With no `.mtl` read, both names mint the same unbound openpbr material — and the library is content-addressed, so
    // the two names share ONE id.
    // The slots stay separate anyway, which is exactly what `asset_material::meshes` is for.
    CHECK(a.material("red") == a.material("blue"));
    CHECK(a.materials[0].meshes.size() == 1);
    CHECK(a.materials[1].meshes.size() == 1);

    // The names are namespaced in the library, so two files each with a "red" do not fight over the name.
    CHECK(lib.get(a.material("red")).name == "quad.obj/red");

    CHECK(a.find_mesh("blue") == &a.meshes[1]);
    CHECK(a.find_mesh("green") == nullptr);
    CHECK(a.meshes_with_material("red").size() == 1);

    auto const box = a.bounds();
    REQUIRE(box.has_value());
    CHECK(box.value().min == tg::pos3f(0, 0, 0));
    CHECK(box.value().max == tg::pos3f(1, 1, 0));
}

TEST("sv::asset_data - override_material moves every mesh bound to the slot")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib});

    auto const doc = babel::obj::read(two_material_obj);
    REQUIRE(doc.has_value());
    auto asset = loader.load(doc.value(), "quad.obj");
    REQUIRE(asset.has_value());
    auto& a = asset.value();

    // Bound to something, so it is a different material rather than the same content-addressed id the import minted.
    auto overrides = cc::vector<sv::material_attribute_binding>();
    overrides.push_back(sv::material_attribute_binding::of("transmission_weight", 1.0f));
    auto const glass = lib.acquire(
        sv::material::create("my-glass", lib.acquire_type(sv::builtin_material::openpbr).value(), overrides));

    // The two slots shared one id before this, so a rewrite keyed on the id would have moved both.
    REQUIRE(a.material("red") == a.material("blue"));
    CHECK(a.override_material("red", glass) == 1);
    CHECK(a.meshes[0].material == glass);
    CHECK(a.meshes[1].material != glass); // the other slot is untouched

    // The slot keeps its name and now names the replacement, so a second override finds it again.
    CHECK(a.material("red") == glass);
    CHECK(a.override_material("nothing-called-this", glass) == 0);
}

TEST("sv::asset_loader - a glTF import places one mesh per primitive, mapped onto openpbr")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib});

    auto const json = triangle_gltf(R"({"attributes": {"POSITION": 1}, "indices": 0, "material": 0})",
                                    R"("materials": [{"name": "glass",
                                        "pbrMetallicRoughness": {"baseColorFactor": [0.1, 0.2, 0.3, 0.5],
                                                                 "metallicFactor": 0.25,
                                                                 "roughnessFactor": 0.75},
                                        "alphaMode": "BLEND"}],)");
    auto const doc = babel::gltf::read(json);
    REQUIRE(doc.has_value());

    auto const asset = loader.load(doc.value(), "car.glb");
    REQUIRE(asset.has_value());
    auto const& a = asset.value();

    REQUIRE(a.meshes.size() == 1);
    CHECK(a.meshes[0].name == "tri"); // one primitive, so the name carries no index to disambiguate
    CHECK(a.meshes[0].geometry.triangle_count() == 1);
    CHECK(a.meshes[0].geometry.vertex_count() == 3);

    // The node's transform is folded into the mesh, which is what "flattened by default" means.
    CHECK(tg::distance(tg::pos3f(0, 0, 0).transformed(a.meshes[0].transform), tg::pos3f(1, 2, 3)) < 1e-5f);

    // The hierarchy is kept as a record whatever the flattening did.
    REQUIRE(a.nodes.size() == 1);
    CHECK(a.nodes[0].name == "root");
    CHECK(a.nodes[0].parent == -1);
    CHECK(a.nodes[0].mesh_count == 1);

    // Only the material the file named: nothing draws with glTF's default here, so the asset does not list one.
    REQUIRE(a.materials.size() == 1);
    CHECK(a.materials[0].name == "glass");
    CHECK(a.meshes[0].material == a.material("glass"));
    CHECK(lib.get(a.material("glass")).name == "car.glb/glass");

    auto const& material = lib.get(a.material("glass"));
    CHECK(material.find("base_color") != nullptr);
    CHECK(material.find("base_metalness") != nullptr);
    CHECK(material.find("specular_roughness") != nullptr);
    // BLEND binds opacity continuously and binds no threshold; MASK is the one that steps.
    CHECK(material.find("opacity") != nullptr);
    CHECK(material.find("alpha_cutoff") == nullptr);
}

TEST("sv::asset_loader - a glTF normal map and occlusion strength are sample transforms")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib});

    // A 1x1 PNG is the smallest thing the image decoder will actually accept, and the import has to decode to bind.
    auto const png = babel::image::encode({.width = 1,
                                           .height = 1,
                                           .channels = 4,
                                           .comp = babel::image::component::u8,
                                           .pixels = cc::vector<byte>::create_filled(4, byte(128))},
                                          babel::image::format::png);
    REQUIRE(png.has_value());

    auto const json = triangle_gltf(R"({"attributes": {"POSITION": 1, "TEXCOORD_0": 2}, "indices": 0, "material": 0})",
                                    cc::format(R"("images": [{{"uri": "data:image/png;base64,{}"}}],
                      "textures": [{{"source": 0}}],
                      "materials": [{{"name": "bumpy",
                                     "normalTexture": {{"index": 0, "scale": 0.5}},
                                     "occlusionTexture": {{"index": 0, "strength": 0.25}}}}],)",
                                               babel::base64::encode(png.value())));

    auto const doc = babel::gltf::read(json);
    REQUIRE(doc.has_value());

    auto const asset = loader.load(doc.value(), "bump.gltf");
    REQUIRE(asset.has_value());
    REQUIRE(asset.value().meshes.size() == 1);

    auto const& textures = asset.value().meshes[0].textures;
    auto const* const normal = [&]() -> sv::mesh_texture_data const*
    {
        for (auto const& t : textures)
            if (t.name == "normal")
                return &t;
        return nullptr;
    }();
    REQUIRE(normal != nullptr);

    // [0,1] read as [-1,1], with normalTexture.scale folded into the same two numbers.
    CHECK(normal->source.transform == sv::sample_transform::of_signed_normal(0.5f));
    CHECK(normal->source.transform.scale[0] == 1.0f); // 2 * 0.5
    CHECK(normal->source.transform.bias[0] == -0.5f);
    CHECK(normal->source.transform.scale[2] == 2.0f); // the normal's own axis is not scaled by it
    // Directions rather than colors, so the texture is linear.
    CHECK(normal->source.texture.format == sg::pixel_format::rgba8_unorm);

    auto const* const occlusion = [&]() -> sv::mesh_texture_data const*
    {
        for (auto const& t : textures)
            if (t.name == "occlusion")
                return &t;
        return nullptr;
    }();
    REQUIRE(occlusion != nullptr);
    CHECK(occlusion->source.transform == sv::sample_transform::of_strength(0.25f));
    CHECK(occlusion->source.swizzle == sv::channel_swizzle::of_channel(sv::texture_channel::r));

    // Both read the same image, so they share one decode — and the same content hash, since both are linear.
    CHECK(normal->source.texture.hash == occlusion->source.texture.hash);

    // The uv set the maps sample through came across too, under the name every material looks for.
    auto has_uv = false;
    for (auto const& attr : asset.value().meshes[0].attributes)
        has_uv = has_uv || attr.name == "uv";
    CHECK(has_uv);

    // Nothing was dropped, so nothing was reported.
    CHECK(asset.value().issues.empty());
}

TEST("sv::asset_loader - a mesh with several primitives becomes several meshes")
{
    auto lib = make_library();

    auto const json = triangle_gltf(R"({"attributes": {"POSITION": 1}, "indices": 0},
                                       {"attributes": {"POSITION": 1}, "indices": 0, "material": 0})",
                                    R"("materials": [{"name": "paint", "alphaMode": "MASK", "alphaCutoff": 0.25}],)");
    auto const doc = babel::gltf::read(json);
    REQUIRE(doc.has_value());

    {
        auto const loader = sv::asset_loader({.materials = &lib});
        auto const asset = loader.load(doc.value(), "car.glb");
        REQUIRE(asset.has_value());
        auto const& a = asset.value();

        // Two primitives, so the names carry the index that tells them apart.
        REQUIRE(a.meshes.size() == 2);
        CHECK(a.meshes[0].name == "tri.0");
        CHECK(a.meshes[1].name == "tri.1");

        // The first names no material, so glTF's own default is minted for it — and only then.
        CHECK(a.material("default") != sv::material_id::invalid);
        CHECK(a.meshes[0].material == a.material("default"));
        CHECK(a.meshes[1].material == a.material("paint"));

        // MASK is a step, so it binds the threshold as well as the value stepped against it.
        CHECK(lib.get(a.material("paint")).find("alpha_cutoff") != nullptr);
    }

    // `include_mesh` runs before a primitive's payloads are read, which is why it is a load option rather than a filter
    // over the result.
    {
        auto const loader
            = sv::asset_loader({.materials = &lib, .include_mesh = [](cc::string_view name) { return name == "tri.1"; }});
        auto const asset = loader.load(doc.value(), "car.glb");
        REQUIRE(asset.has_value());
        REQUIRE(asset.value().meshes.size() == 1);
        CHECK(asset.value().meshes[0].name == "tri.1");
    }
}

TEST("sv::asset_loader - the material_override hook replaces a material before one is built")
{
    auto lib = make_library();
    auto const mine
        = lib.acquire(sv::material::create("mine", lib.acquire_type(sv::builtin_material::openpbr).value(), {}));

    auto const loader = sv::asset_loader(
        {.materials = &lib,
         .material_override = [&](cc::string_view name) { return name == "glass" ? mine : sv::material_id::invalid; }});

    auto const json = triangle_gltf(R"({"attributes": {"POSITION": 1}, "indices": 0, "material": 0})",
                                    R"("materials": [{"name": "glass"}],)");
    auto const doc = babel::gltf::read(json);
    REQUIRE(doc.has_value());

    auto const asset = loader.load(doc.value(), "car.glb");
    REQUIRE(asset.has_value());
    CHECK(asset.value().material("glass") == mine);
    CHECK(asset.value().meshes[0].material == mine);
}

TEST("sv::asset_loader - the uri path runs entirely through the resolver")
{
    auto lib = make_library();

    // No filesystem anywhere: the loader asks for bytes and this is what answers.
    auto const resolve = [](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
    {
        if (uri != "mem/quad.obj")
            return cc::error(cc::format("no such uri: {}", uri));

        auto text = cc::vector<byte>();
        for (auto const c : two_material_obj)
            text.push_back(byte(c));
        return cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(text)));
    };

    auto const loader = sv::asset_loader({.materials = &lib, .resolve = resolve});

    auto const asset = loader.load("mem/quad.obj");
    REQUIRE(asset.has_value());
    CHECK(asset.value().meshes.size() == 2);
    CHECK(asset.value().name == "mem/quad.obj");

    // A uri the resolver refuses is an error rather than an empty asset.
    CHECK(loader.load("mem/missing.obj").has_error());
    // ...and so is one naming a format this loader does not read, before the resolver is even asked.
    CHECK(loader.load("mem/quad.fbx").has_error());
}

TEST("sv::asset_loader - import_materials off leaves every mesh on the default material")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib, .import_materials = false});

    auto const doc = babel::obj::read(two_material_obj);
    REQUIRE(doc.has_value());
    auto const asset = loader.load(doc.value(), "quad.obj");
    REQUIRE(asset.has_value());

    CHECK(asset.value().meshes.size() == 2);
    CHECK(asset.value().materials.empty());
    for (auto const& m : asset.value().meshes)
        CHECK(m.material == sv::material_id::invalid); // which draws with sv::default_material
}

TEST("sv::asset_loader - an STL import is one mesh of raw triangles")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib});

    constexpr cc::string_view ascii_stl = R"stl(solid widget
facet normal 0 0 1
outer loop
vertex 0 0 0
vertex 1 0 0
vertex 1 1 0
endloop
endfacet
facet normal 0 0 1
outer loop
vertex 0 0 0
vertex 1 1 0
vertex 0 1 0
endloop
endfacet
endsolid widget
)stl";

    auto const doc = babel::stl::read(ascii_stl);
    REQUIRE(doc.has_value());

    auto const asset = loader.load(doc.value(), "widget.stl");
    REQUIRE(asset.has_value());
    auto const& a = asset.value();

    // STL carries no materials and no hierarchy, so one file is one mesh — named after the solid rather than the uri.
    REQUIRE(a.meshes.size() == 1);
    CHECK(a.meshes[0].name == "widget");
    CHECK(a.materials.empty());
    CHECK(a.meshes[0].material == sv::material_id::invalid);

    // A raw triangle list, kept as the soup it already is: six positions rather than four welded ones.
    CHECK(!a.meshes[0].geometry.is_indexed());
    CHECK(a.meshes[0].geometry.triangle_count() == 2);
    CHECK(a.meshes[0].geometry.vertex_count() == 6);

    // The per-facet normals are deliberately not imported: the hit shader derives the geometric frame from the
    // triangle, and a per-triangle normal could only ever match it.
    CHECK(a.meshes[0].attributes.empty());

    CHECK(sv::asset_format_of_uri("widget.stl").value() == sv::asset_format::stl);
}

TEST("sv::asset - an async load lands whole, and its materials are minted where the library is")
{
    auto lib = make_library();

    // No filesystem: the load's fetch stage goes through this, on whatever thread runs it.
    auto const resolve = [](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
    {
        if (uri != "mem/quad.obj")
            return cc::error(cc::format("no such uri: {}", uri));

        auto text = cc::vector<byte>();
        for (auto const c : two_material_obj)
            text.push_back(byte(c));
        return cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(text)));
    };

    auto const loader = sv::asset_loader({.materials = &lib, .resolve = resolve});

    auto pending = loader.load_async("mem/quad.obj");
    CHECK(pending.is_valid());

    // Nothing is available until a poll sees it land, and asking never blocks.
    CHECK(!pending.is_ready());
    CHECK(pending.meshes().empty());

    // `wait` drives the load wherever it is — already done on a pool, or not started at all with none installed.
    CHECK(pending.wait());
    CHECK(pending.is_ready());
    CHECK(!pending.has_error());

    // The same asset the synchronous path produces, materials included — which is the point of splitting the import
    // rather than only moving it.
    REQUIRE(pending.meshes().size() == 2);
    CHECK(pending.meshes()[0].name == "red");
    CHECK(pending.data().material("red") != sv::material_id::invalid);
    CHECK(pending.meshes()[0].material == pending.data().material("red"));
    CHECK(lib.get(pending.data().material("red")).name == "mem/quad.obj/red");

    // Polling again is idempotent, which is what lets a frame loop call it unconditionally.
    CHECK(pending.poll());
    CHECK(pending.meshes().size() == 2);
}

TEST("sv::asset - a glTF's structure lands before its payloads, as boxes with keys")
{
    auto lib = make_library();

    auto const json = triangle_gltf(R"({"attributes": {"POSITION": 1}, "indices": 0, "material": 0})",
                                    R"("materials": [{"name": "glass"}],)");

    auto const resolve = [&](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
    {
        if (uri != "mem/car.gltf")
            return cc::error(cc::format("no such uri: {}", uri));

        auto text = cc::vector<byte>();
        for (auto const c : cc::string_view(json))
            text.push_back(byte(c));
        return cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(text)));
    };

    auto const loader = sv::asset_loader({.materials = &lib, .resolve = resolve});
    auto car = loader.load_async("mem/car.gltf");

    // Driving only the structure half is what a caller who polls before the payloads land sees.
    car.wait();
    REQUIRE(car.is_ready());
    CHECK(car.is_complete());

    // Whichever half is showing, the mesh list is the same one — placed, named and sized.
    REQUIRE(car.meshes().size() == 1);
    CHECK(car.meshes()[0].name == "tri");
    REQUIRE(car.meshes()[0].bounds.has_value());
    CHECK(car.meshes()[0].bounds.value().max == tg::pos3f(1, 1, 0));
    CHECK(car.data().material("glass") != sv::material_id::invalid);

    // The completed half carries the payload, and it answers under the RECIPE key rather than a content hash — which
    // is what lets the promise and the payload be one record.
    CHECK(!car.meshes()[0].geometry.is_deferred());
    CHECK(car.meshes()[0].geometry.triangle_count() == 1);
    CHECK(car.meshes()[0].geometry.hash == sv::impl::gltf_primitive_key("mem/car.gltf", 0, 0));
}

TEST("sv::asset_loader - a structure import reads no payload byte, and promises its geometry")
{
    auto lib = make_library();
    auto const loader = sv::asset_loader({.materials = &lib});

    auto const json = triangle_gltf(R"({"attributes": {"POSITION": 1}, "indices": 0, "material": 0})",
                                    R"("materials": [{"name": "glass"}],)");
    auto const doc = babel::gltf::read(json);
    REQUIRE(doc.has_value());

    auto definitions = cc::vector<sv::impl::asset_material_definition>();
    auto const structure
        = sv::impl::import_gltf(doc.value(), loader.config(), "car.gltf", definitions, sv::impl::import_mode::structure);
    REQUIRE(structure.has_value());

    REQUIRE(structure.value().meshes.size() == 1);
    auto const& mesh = structure.value().meshes[0];

    // A promise, not bytes — and not nothing either, which is the distinction `is_empty` has to keep.
    CHECK(mesh.geometry.is_deferred());
    CHECK(!mesh.geometry.is_empty());
    CHECK(mesh.geometry.positions.empty());
    CHECK(mesh.geometry.hash == sv::impl::gltf_primitive_key("car.gltf", 0, 0));

    // Everything a placeholder needs came out of the JSON: where it goes, how big it is, what it is made of.
    CHECK(mesh.name == "tri");
    REQUIRE(mesh.bounds.has_value());
    CHECK(mesh.bounds.value().max == tg::pos3f(1, 1, 0));
    CHECK(tg::distance(tg::pos3f(0, 0, 0).transformed(mesh.transform), tg::pos3f(1, 2, 3)) < 1e-5f);
    REQUIRE(definitions.size() == 1);
    CHECK(definitions[0].name == "glass");

    // And nothing that costs an accessor read did: no attributes, no uv set, no decoded image.
    CHECK(mesh.attributes.empty());
    CHECK(mesh.textures.empty());

    // The payload half agrees about the key, which is the whole contract between the two.
    auto payload_definitions = cc::vector<sv::impl::asset_material_definition>();
    auto const payloads = sv::impl::import_gltf(doc.value(), loader.config(), "car.gltf", payload_definitions,
                                                sv::impl::import_mode::payloads);
    REQUIRE(payloads.has_value());
    REQUIRE(payloads.value().meshes.size() == 1);
    CHECK(payloads.value().meshes[0].geometry.hash == mesh.geometry.hash);
    CHECK(!payloads.value().meshes[0].geometry.is_deferred());
}

TEST("sv::asset - a load that cannot be resolved reports it rather than throwing")
{
    auto lib = make_library();

    auto const resolve = [](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
    { return cc::error(cc::format("no such uri: {}", uri)); };

    auto const loader = sv::asset_loader({.materials = &lib, .resolve = resolve});

    auto missing = loader.load_async("mem/missing.obj");
    CHECK(!missing.wait());
    CHECK(missing.has_error());
    CHECK(!missing.is_ready());
    CHECK(missing.meshes().empty());

    // A uri naming no format this loader reads fails the same way, without the resolver being asked.
    auto unknown = loader.load_async("mem/quad.fbx");
    CHECK(!unknown.wait());
    CHECK(unknown.has_error());
}
