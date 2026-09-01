#include <babel-serializer/data/base64.hh>
#include <clean-core/common/utility.hh> // cc::memcpy, cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/record/log.hh> // CC_LOG_ERROR, CC_LOG_WARNING
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>

using namespace cc::primitive_defines;

// Loading a file and drawing what came out of it.
//
// `sv::asset_loader` holds no device and opens no file: it turns bytes into `sv::asset_data`, which is made of the very
// types `add_mesh` already takes.
// So there is no bridge here — the meshes an import produced are placed directly.
//
// The document is BUILT IN CODE and served through `sv::set_resolve_uri`, for two reasons.
// It keeps a binary asset out of the repository for one example, which is the pattern babel's own geometry tests
// follow.
// And it is the honest demonstration of the resolver seam: the loader never learns that "temple.gltf" is not a path,
// because nothing in the importer opens a file.
//
// What the picture shows: a floor, four pillars and a gold roof — all six of them ONE uploaded box.
// The glTF references a single POSITION accessor from both of its meshes and places them with six nodes, so the
// geometry is content-hashed to one upload and drawn six times with six transforms — instancing the importer gets for
// free rather than implements.
//
// Controls
//   left-drag    orbit          middle-drag    pan          wheel    zoom
//
// Run it:
//   uv run dev.py example shaped-viewer/load-asset

namespace
{
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

/// The unit box as glTF binary: 36 u16 indices, then 8 vec3 corners of the cube spanning 0..1 on every axis.
///
/// No NORMAL accessor, on purpose: the hit shader computes the geometric frame from the triangle it already has, so
/// importing per-face normals would spend memory to store what is free.
[[nodiscard]] cc::vector<byte> unit_box_bin()
{
    // Corner i: bit 0 is +x, bit 1 is +y, bit 2 is +z. Each quad is wound so its normal points outward.
    u16 const quads[6][4] = {
        {1, 3, 7, 5}, // +x
        {0, 4, 6, 2}, // -x
        {2, 6, 7, 3}, // +y
        {0, 1, 5, 4}, // -y
        {4, 5, 7, 6}, // +z
        {0, 2, 3, 1}, // -z
    };

    auto out = cc::vector<byte>();
    for (auto const& q : quads)
        for (auto const i : {0, 1, 2, 0, 2, 3})
            push_le_u16(out, q[i]);

    for (auto i = 0; i < 8; ++i)
    {
        push_le_f32(out, (i & 1) ? 1.0f : 0.0f);
        push_le_f32(out, (i & 2) ? 1.0f : 0.0f);
        push_le_f32(out, (i & 4) ? 1.0f : 0.0f);
    }
    return out;
}

/// A `.gltf` document: two meshes over ONE box accessor, five nodes placing them, two metallic-roughness materials.
///
/// Both meshes name the same POSITION accessor, so the importer produces two meshes whose geometry hashes identically
/// — one upload, five instances.
/// What differs is the material each names, which is what makes them two meshes rather than one.
[[nodiscard]] cc::string temple_gltf()
{
    auto const bin = unit_box_bin();

    auto json = cc::string();
    json += R"({"asset": {"version": "2.0"},
        "buffers": [{"uri": "data:application/octet-stream;base64,)";
    json += babel::base64::encode(bin);
    json += cc::format(R"(", "byteLength": {}}}],)", bin.size());
    json += R"(
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0,  "byteLength": 72},
            {"buffer": 0, "byteOffset": 72, "byteLength": 96}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5123, "count": 36, "type": "SCALAR"},
            {"bufferView": 1, "componentType": 5126, "count": 8,  "type": "VEC3",
             "min": [0, 0, 0], "max": [1, 1, 1]}
        ],
        "materials": [
            {"name": "stone", "pbrMetallicRoughness": {
                "baseColorFactor": [0.42, 0.41, 0.38, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.85}},
            {"name": "gold", "pbrMetallicRoughness": {
                "baseColorFactor": [1.00, 0.78, 0.36, 1.0], "metallicFactor": 1.0, "roughnessFactor": 0.42}}
        ],
        "meshes": [
            {"name": "pillar", "primitives": [{"attributes": {"POSITION": 1}, "indices": 0, "material": 0}]},
            {"name": "roof",   "primitives": [{"attributes": {"POSITION": 1}, "indices": 0, "material": 1}]}
        ],
        "nodes": [
            {"name": "floor",     "mesh": 0, "translation": [-6.0, -0.4, -6.0], "scale": [12.0, 0.4, 12.0]},
            {"name": "pillar.nw", "mesh": 0, "translation": [-1.6, 0, -1.6], "scale": [0.5, 3.0, 0.5]},
            {"name": "pillar.ne", "mesh": 0, "translation": [ 1.1, 0, -1.6], "scale": [0.5, 3.0, 0.5]},
            {"name": "pillar.sw", "mesh": 0, "translation": [-1.6, 0,  1.1], "scale": [0.5, 3.0, 0.5]},
            {"name": "pillar.se", "mesh": 0, "translation": [ 1.1, 0,  1.1], "scale": [0.5, 3.0, 0.5]},
            {"name": "roof",      "mesh": 1, "translation": [-2.0, 3.0, -2.0], "scale": [4.0, 0.4, 4.0]}
        ],
        "scenes": [{"nodes": [0, 1, 2, 3, 4, 5], "name": "temple"}],
        "scene": 0})";
    return json;
}
} // namespace

EXAMPLE("shaped-viewer/load-asset")
{
    // The whole filesystem this example has.
    // A host with a real virtual filesystem replaces the hook exactly here, process-wide, rather than threading a
    // resolver through every call.
    auto const document = temple_gltf();
    sv::set_resolve_uri(
        [&document](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
        {
            if (uri != "temple.gltf")
                return cc::error(cc::format("this example serves only 'temple.gltf', not '{}'", uri));

            auto bytes = cc::vector<byte>::create_uninitialized(document.size());
            cc::memcpy(bytes.data(), document.data(), size_t(document.size()));
            return cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(bytes)));
        });

    // Held by value for the loop's lifetime: the loader is neither movable nor copyable, because an async load reaches
    // back into it.
    auto const loader = sv::asset_loader();

    auto const asset = loader.load("temple.gltf");
    if (asset.has_error())
    {
        CC_LOG_ERROR("load-asset: could not load the asset: {}", asset.error().to_string());
        return;
    }

    // A successful load with issues is the normal case for a real asset, so this is worth printing rather than
    // checking: it is what says which primitive was skipped, or which map did not decode.
    for (auto const& issue : asset.value().issues)
        CC_LOG_WARNING("load-asset: {}", issue);

    // What a camera is framed on.
    // The importer took it from the accessors' own `min` / `max`, so nothing scanned a vertex to answer this.
    auto const box = asset.value().bounds();
    auto const center
        = box.has_value() ? box.value().min + (box.value().max - box.value().min) * 0.5f : tg::pos3f(0, 0, 0);

    // Framed on the structure rather than on the whole box: the floor is wide and would push the camera far enough back
    // to make the subject small.
    auto const target = tg::pos3d(double(center[0]), 1.6, double(center[2]));

    for (auto f : sv::interactive("shaped-viewer/load-asset"))
    {
        auto view = f.window().view();
        view.initial_orbit({.target = target,
                            .distance = 9.5,
                            .azimuth = tg::angle_d::make_from_degree(38.0),
                            .elevation = tg::angle_d::make_from_degree(14.0)});

        auto scene = view.add_scene();

        // Placed directly: an imported mesh IS an `sv::mesh`, so there is nothing to convert.
        // Re-placing them every frame uploads nothing after the first — every payload is keyed by its content hash,
        // and each mesh remembers what the last placement produced.
        for (auto const& mesh : asset.value().meshes)
            scene.add_mesh(mesh);

        // Behind and to one side rather than overhead, for two reasons.
        // The roof is metallic, and a metal is only legible where something with structure reflects in it — here the
        // lit floor and the sky gradient.
        // And it throws the pillars' shadows toward the camera, which is what makes them read as standing on the floor
        // rather than hovering over it.
        // The emission falls as the rect grows, which is the same constant openpbr-spheres uses: what matters is that
        // the light dominates the sky, not that the scene is bright.
        float const light_u = 2.0f;
        float const light_v = 2.0f;
        scene.add_light({.center = tg::pos3f(-2.4f, 7.0f, 1.0f),
                         .half_extent_u = tg::vec3f(light_u, 0, 0),
                         .half_extent_v = tg::vec3f(0, 0, light_v),
                         .emission = tg::vec3f(1.0f, 0.97f, 0.92f) * (90.0f / (light_u * light_v))});

        // Deliberately dim: a bright sky is a dome light, and a dome fills the space under the roof until the pillars
        // cast no shadow anyone can see.
        scene.background(sv::background::gradient(tg::vec3f(0.30f, 0.39f, 0.55f), tg::vec3f(0.10f, 0.12f, 0.15f)));
    }
}
