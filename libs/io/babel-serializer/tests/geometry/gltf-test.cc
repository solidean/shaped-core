#include <babel-serializer/data/base64.hh>
#include <babel-serializer/geometry/gltf.hh>
#include <clean-core/common/utility.hh> // cc::memcpy, cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/streams/span_stream.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr u32 glb_magic = 0x46546C67;
constexpr u32 glb_chunk_json = 0x4E4F534A;
constexpr u32 glb_chunk_bin = 0x004E4942;

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

void push_bytes(cc::vector<byte>& out, cc::span<byte const> bytes)
{
    for (auto const b : bytes)
        out.push_back(b);
}

f32 read_le_f32(cc::span<byte const> bytes, isize offset)
{
    auto bits = u32(0);
    for (auto k = 0; k < 4; ++k)
        bits |= u32(u8(bytes[offset + k])) << (8 * k);
    auto value = f32(0);
    cc::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// The binary payload every geometry test shares: three u16 indices, two padding bytes so the positions
/// start 4-byte aligned, then three vec3 f32 positions.
/// 44 bytes in total.
cc::vector<byte> triangle_bin()
{
    auto out = cc::vector<byte>();
    push_le_u16(out, 0);
    push_le_u16(out, 1);
    push_le_u16(out, 2);
    out.push_back(byte(0));
    out.push_back(byte(0));

    for (auto const v : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f})
        push_le_f32(out, v);

    return out;
}

/// The document that goes with triangle_bin, with `buffer_spec` filling in the buffer entry.
cc::string triangle_json(cc::string_view buffer_spec)
{
    auto json = cc::string();
    json += R"({"asset": {"version": "2.0"}, "buffers": [)";
    json += buffer_spec;
    json += R"(],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 6, "target": 34963},
            {"buffer": 0, "byteOffset": 8, "byteLength": 36, "target": 34962}
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5123, "count": 3, "type": "SCALAR"},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3",
             "min": [0, 0, 0], "max": [1, 1, 0]}
        ],
        "meshes": [{"name": "tri", "primitives": [{"attributes": {"POSITION": 1}, "indices": 0}]}],
        "nodes": [{"mesh": 0, "name": "root"}],
        "scenes": [{"nodes": [0], "name": "main"}],
        "scene": 0})";
    return json;
}

cc::string base64_buffer_spec(cc::span<byte const> bin)
{
    auto spec = cc::string();
    spec += R"({"uri": "data:application/octet-stream;base64,)";
    spec += babel::base64::encode(bin);
    spec += R"(", "byteLength": )";
    spec += cc::format("{}", bin.size());
    spec += "}";
    return spec;
}

/// Assemble a GLB: the 12-byte header, then the JSON chunk (space-padded) and, for a non-empty `bin`,
/// the BIN chunk (zero-padded). Both paddings live inside the chunk length, exactly as the spec requires.
cc::vector<byte> make_glb(cc::string_view json, cc::span<byte const> bin)
{
    auto const json_padded = (json.size() + 3) / 4 * 4;
    auto const bin_padded = (bin.size() + 3) / 4 * 4;
    auto total = isize(12 + 8) + json_padded;
    if (!bin.empty())
        total += 8 + bin_padded;

    auto out = cc::vector<byte>();
    push_le_u32(out, glb_magic);
    push_le_u32(out, 2);
    push_le_u32(out, u32(total));

    push_le_u32(out, u32(json_padded));
    push_le_u32(out, glb_chunk_json);
    push_bytes(out, json.as_bytes());
    for (auto i = json.size(); i < json_padded; ++i)
        out.push_back(byte(' '));

    if (!bin.empty())
    {
        push_le_u32(out, u32(bin_padded));
        push_le_u32(out, glb_chunk_bin);
        push_bytes(out, bin);
        for (auto i = bin.size(); i < bin_padded; ++i)
            out.push_back(byte(0));
    }

    return out;
}

isize count_issues(babel::gltf::data const& doc, babel::gltf::issue_kind kind)
{
    auto n = isize(0);
    for (auto const& issue : doc.issues)
        if (issue.kind == kind)
            ++n;
    return n;
}

cc::vector<byte> glb_header(u32 version, u32 length)
{
    auto out = cc::vector<byte>();
    push_le_u32(out, glb_magic);
    push_le_u32(out, version);
    push_le_u32(out, length);
    return out;
}
} // namespace

TEST("gltf - minimal json document")
{
    auto const doc = babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0", "generator": "nexus"}})")).value();

    CHECK(doc.source == babel::gltf::container::gltf);
    CHECK(doc.asset.version == "2.0");
    CHECK(doc.asset.generator == "nexus");
    CHECK(doc.asset.min_version == "");
    CHECK(doc.buffers.empty());
    CHECK(doc.meshes.empty());
    CHECK(doc.nodes.empty());
    CHECK(doc.default_scene == babel::gltf::scene_index::invalid);
    CHECK(!doc.has_issues()); // nothing skipped, nothing unresolved, nothing tolerated
    CHECK(doc.issue_report() == "");
}

TEST("gltf - a triangle from a base64 data uri buffer")
{
    auto const bin = triangle_bin();
    auto const json = triangle_json(base64_buffer_spec(bin));
    auto const doc = babel::gltf::read(cc::string_view(json)).value();

    REQUIRE(doc.buffers.size() == 1);
    CHECK(doc.buffers[0].resolved);
    CHECK(doc.buffers[0].byte_length == 44);
    CHECK(doc.buffers[0].data.size() == 44);

    REQUIRE(doc.meshes.size() == 1);
    CHECK(doc.meshes[0].name == "tri");

    auto const primitives = doc.primitives_of(doc.meshes[0]);
    REQUIRE(primitives.size() == 1);
    CHECK(primitives[0].mode == babel::gltf::primitive_mode::triangles); // the default when `mode` is absent
    CHECK(primitives[0].material == babel::gltf::material_index::invalid);

    auto const attributes = doc.attributes_of(primitives[0]);
    REQUIRE(attributes.size() == 1);
    CHECK(attributes[0].semantic == "POSITION");

    auto const positions = doc.find_attribute(primitives[0], "POSITION");
    CHECK(positions == babel::gltf::accessor_index(1));
    CHECK(doc.find_attribute(primitives[0], "NORMAL") == babel::gltf::accessor_index::invalid);

    auto const view = doc.view_of(positions).value();
    CHECK(view.count == 3);
    CHECK(view.element_size == 12);
    CHECK(view.stride == 12); // the bufferView states no byteStride, so the elements are tightly packed

    auto const points = view.read_elements<tg::vec3f>().value();
    REQUIRE(points.size() == 3);
    CHECK(points[0] == tg::vec3f::zero);
    CHECK(points[1] == tg::vec3f(1, 0, 0));
    CHECK(points[2] == tg::vec3f(0, 1, 0));

    // Whatever the pin's alignment, a type of the wrong size is never readable in place.
    CHECK(!view.is_typed_as<tg::vec2f>());
    CHECK(view.read_elements<tg::vec2f>().has_error());
    CHECK(view.element(0).size() == 12);
    CHECK(view.element(2).size() == 12);

    if (view.is_typed_as<tg::vec3f>())
    {
        auto seen = isize(0);
        for (auto const& p : view.as_strided<tg::vec3f>())
        {
            CHECK(p == points[seen]);
            ++seen;
        }
        CHECK(seen == 3);
    }

    auto const indices = doc.read_indices(primitives[0]).value();
    REQUIRE(indices.size() == 3);
    CHECK(indices[0] == 0);
    CHECK(indices[1] == 1);
    CHECK(indices[2] == 2);

    auto const& accessor = doc.accessors[1];
    auto const mins = doc.min_of(accessor);
    auto const maxs = doc.max_of(accessor);
    REQUIRE(mins.size() == 3);
    REQUIRE(maxs.size() == 3);
    CHECK(mins[0] == 0);
    CHECK(maxs[0] == 1);
    CHECK(maxs[1] == 1);
    CHECK(maxs[2] == 0);

    REQUIRE(doc.scenes.size() == 1);
    CHECK(doc.default_scene == babel::gltf::scene_index(0));
    CHECK(doc.nodes_of(doc.scenes[0]).size() == 1);

    // A file this reader fully understands must come back with a clean issue list.
    CHECK(!doc.has_issues());
}

TEST("gltf - glb container with a bin chunk")
{
    auto const glb = make_glb(triangle_json(R"({"byteLength": 44})"), triangle_bin());
    auto const doc = babel::gltf::read(cc::span<byte const>(glb)).value();

    CHECK(doc.source == babel::gltf::container::glb);
    REQUIRE(doc.buffers.size() == 1);
    CHECK(doc.buffers[0].uri == ""); // the BIN chunk is the one buffer that carries no uri
    CHECK(doc.buffers[0].resolved);
    CHECK(doc.buffers[0].byte_length == 44);
    CHECK(doc.buffers[0].data.size() == 44);

    auto const indices = doc.read_indices(doc.primitives_of(doc.meshes[0])[0]).value();
    REQUIRE(indices.size() == 3);
    CHECK(indices[2] == 2);
}

TEST("gltf - a bin buffer is trimmed to byteLength, past the chunk padding")
{
    // 3 bytes of payload pad the BIN chunk out to 4, and `resolved` must still imply the exact byteLength.
    auto bin = cc::vector<byte>();
    bin.push_back(byte(0xAA));
    bin.push_back(byte(0xBB));
    bin.push_back(byte(0xCC));

    auto const glb = make_glb(R"({"asset": {"version": "2.0"}, "buffers": [{"byteLength": 3}]})", bin);
    auto const doc = babel::gltf::read(cc::span<byte const>(glb)).value();

    REQUIRE(doc.buffers.size() == 1);
    CHECK(doc.buffers[0].resolved);
    CHECK(doc.buffers[0].data.size() == 3);
    CHECK(doc.buffers[0].data[2] == byte(0xCC));

    // A BIN chunk shorter than the declared byteLength is broken, not merely padded.
    auto const short_chunk = make_glb(R"({"asset": {"version": "2.0"}, "buffers": [{"byteLength": 9}]})", bin);
    CHECK(babel::gltf::read(cc::span<byte const>(short_chunk)).has_error());
}

TEST("gltf - glb bin buffer is zero-copy and outlives the input handle")
{
    auto glb = make_glb(triangle_json(R"({"byteLength": 44})"), triangle_bin());
    auto const expected_bin_offset = isize(glb.size() - 44); // the BIN chunk is last and needs no padding

    auto buffer_bytes = cc::pinned_data<byte const>();
    byte const* input_begin = nullptr;

    {
        auto const pinned = cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(glb)));
        input_begin = pinned.data();

        auto const doc = babel::gltf::read(pinned).value();
        REQUIRE(doc.buffers.size() == 1);

        // Inside the input, and sharing its owner: no allocation happened for the payload at all.
        CHECK(doc.buffers[0].data.data() == input_begin + expected_bin_offset);
        CHECK(doc.buffers[0].data.pin().get() == pinned.pin().get());

        buffer_bytes = doc.buffers[0].data; // a refcount bump, not a copy
    }

    // Both the input handle and the document are gone; only buffer_bytes keeps the allocation alive.
    // If the reader ever copied-then-freed, this read is a use-after-free a sanitizer would catch.
    REQUIRE(buffer_bytes.size() == 44);
    CHECK(buffer_bytes.data() == input_begin + expected_bin_offset);
    CHECK(read_le_f32(buffer_bytes, 8 + 12) == 1.0f); // the second position's x
}

TEST("gltf - the span overload copies the input bytes")
{
    auto const glb = make_glb(triangle_json(R"({"byteLength": 44})"), triangle_bin());
    auto const doc = babel::gltf::read(cc::span<byte const>(glb)).value();

    REQUIRE(doc.buffers.size() == 1);
    CHECK(doc.buffers[0].resolved);

    // A span is a borrow, so read pins an owned copy — the buffer must NOT point into the caller's memory.
    auto const* const inside = doc.buffers[0].data.data();
    CHECK(!(inside >= glb.data() && inside < glb.data() + glb.size()));
}

TEST("gltf - reads through the read_stream overload")
{
    auto const glb = make_glb(triangle_json(R"({"byteLength": 44})"), triangle_bin());

    auto adapter = cc::span_read_stream_adapter(cc::span<byte const>(glb));
    cc::read_stream stream = adapter;

    auto const doc = babel::gltf::read(stream).value();
    CHECK(doc.source == babel::gltf::container::glb);
    REQUIRE(doc.buffers.size() == 1);
    CHECK(doc.buffers[0].data.size() == 44);
}

TEST("gltf - accessor strides, offsets and interleaving")
{
    // One bufferView of stride 20 holding two interleaved records: vec3 position then vec2 texcoord.
    auto bin = cc::vector<byte>();
    push_le_f32(bin, 0);
    push_le_f32(bin, 0);
    push_le_f32(bin, 0);
    push_le_f32(bin, 0);
    push_le_f32(bin, 0);
    push_le_f32(bin, 1);
    push_le_f32(bin, 2);
    push_le_f32(bin, 3);
    push_le_f32(bin, 0.5f);
    push_le_f32(bin, 0.25f);

    auto json = cc::string();
    json += R"({"asset": {"version": "2.0"}, "buffers": [)";
    json += base64_buffer_spec(bin);
    json += R"(],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 40, "byteStride": 20}],
        "accessors": [
            {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 2, "type": "VEC3"},
            {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 2, "type": "VEC2"}
        ]})";

    auto const doc = babel::gltf::read(cc::string_view(json)).value();
    REQUIRE(doc.accessors.size() == 2);

    auto const positions = doc.view_of(doc.accessors[0]).value();
    CHECK(positions.stride == 20);
    CHECK(positions.element_size == 12);
    // (count - 1) * stride + element_size, so the view stops right after the last element.
    CHECK(positions.bytes.size() == 32);

    auto const points = positions.read_elements<tg::vec3f>().value();
    REQUIRE(points.size() == 2);
    CHECK(points[0] == tg::vec3f::zero);
    CHECK(points[1] == tg::vec3f(1, 2, 3));

    auto const texcoords = doc.view_of(doc.accessors[1]).value();
    CHECK(texcoords.stride == 20);
    CHECK(texcoords.element_size == 8);

    auto const uvs = texcoords.read_elements<tg::vec2f>().value();
    REQUIRE(uvs.size() == 2);
    CHECK(uvs[0] == tg::vec2f::zero);
    CHECK(uvs[1] == tg::vec2f(0.5f, 0.25f));
}

TEST("gltf - accessor element sizes include matrix column padding")
{
    // No bufferView on any of these: the element arithmetic is a property of the accessor alone.
    auto const doc = babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "accessors": [
            {"componentType": 5126, "count": 0, "type": "VEC3"},
            {"componentType": 5126, "count": 0, "type": "MAT4"},
            {"componentType": 5126, "count": 0, "type": "MAT3"},
            {"componentType": 5121, "count": 0, "type": "MAT3"},
            {"componentType": 5123, "count": 0, "type": "MAT3"},
            {"componentType": 5121, "count": 0, "type": "MAT2"},
            {"componentType": 5123, "count": 0, "type": "SCALAR"}
        ]})"))
                         .value();

    REQUIRE(doc.accessors.size() == 7);
    CHECK(doc.accessors[0].element_size() == 12);
    CHECK(doc.accessors[1].element_size() == 64); // == sizeof(tg::mat4f)
    CHECK(doc.accessors[2].element_size() == 36); // == sizeof(tg::mat3f)
    CHECK(doc.accessors[3].element_size() == 12); // 3 columns of 3 u8, each padded to 4 — not 9
    CHECK(doc.accessors[4].element_size() == 24); // 3 columns of 3 u16, each padded to 8 — not 18
    CHECK(doc.accessors[5].element_size() == 8);
    CHECK(doc.accessors[6].element_size() == 2);

    CHECK(doc.accessors[0].component_count() == 3);
    CHECK(doc.accessors[2].component_count() == 9);
    CHECK(doc.accessors[4].component_size() == 2);

    // An accessor without a bufferView has no bytes to hand out.
    CHECK(doc.view_of(doc.accessors[0]).has_error());
}

TEST("gltf - node transforms keep the form the file used")
{
    auto const doc = babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "nodes": [
            {"name": "by matrix", "matrix": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1], "children": [1]},
            {"name": "by trs", "translation": [1, 2, 3], "rotation": [0, 0, 1, 0], "scale": [2, 2, 2]},
            {"name": "default"}
        ],
        "scenes": [{"nodes": [0]}]})"))
                         .value();

    REQUIRE(doc.nodes.size() == 3);

    CHECK(doc.nodes[0].has_matrix);
    // glTF lists the matrix column by column, so the translation sits in column 3.
    CHECK((doc.nodes[0].matrix[3, 0]) == 5);
    CHECK((doc.nodes[0].matrix[3, 1]) == 6);
    CHECK((doc.nodes[0].matrix[3, 2]) == 7);
    CHECK((doc.nodes[0].matrix[0, 0]) == 1);
    CHECK(doc.nodes[0].translation == tg::vec3f::zero); // untouched when `matrix` won
    CHECK(doc.children_of(doc.nodes[0]).size() == 1);
    CHECK(doc.children_of(doc.nodes[0])[0] == babel::gltf::node_index(1));

    CHECK(!doc.nodes[1].has_matrix);
    CHECK(doc.nodes[1].translation == tg::vec3f(1, 2, 3));
    CHECK(doc.nodes[1].rotation == tg::quat_f(0, 0, 1, 0)); // glTF orders a rotation as [x, y, z, w]
    CHECK(doc.nodes[1].scale == tg::vec3f(2, 2, 2));

    CHECK(!doc.nodes[2].has_matrix);
    CHECK(doc.nodes[2].scale == tg::vec3f(1, 1, 1)); // the spec's default scale is one, not zero
    CHECK(doc.nodes[2].rotation == tg::quat_f::identity);
    CHECK(doc.children_of(doc.nodes[2]).empty());
}

TEST("gltf - materials, textures, samplers and images")
{
    auto image_bin = cc::vector<byte>();
    for (auto v = 0; v < 8; ++v)
        image_bin.push_back(byte(u8(0x10 + v)));

    auto json = cc::string();
    json += R"({"asset": {"version": "2.0"},
        "buffers": [{"byteLength": 8}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 8}],
        "materials": [
            {},
            {"name": "full",
             "pbrMetallicRoughness": {
                 "baseColorFactor": [0.1, 0.2, 0.3, 0.4], "metallicFactor": 0.25, "roughnessFactor": 0.75,
                 "baseColorTexture": {"index": 0, "texCoord": 1}},
             "normalTexture": {"index": 0, "scale": 2},
             "occlusionTexture": {"index": 0, "strength": 0.5},
             "emissiveTexture": {"index": 0},
             "emissiveFactor": [1, 0, 0],
             "alphaMode": "MASK", "alphaCutoff": 0.25, "doubleSided": true}
        ],
        "textures": [{"sampler": 0, "source": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071}],
        "images": [{"bufferView": 0, "mimeType": "image/png"}, {"uri": "external.png"}]})";

    auto const glb = make_glb(json, image_bin);
    auto const pinned = cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(glb)));
    auto const doc = babel::gltf::read(pinned).value();

    REQUIRE(doc.materials.size() == 2);

    auto const& plain = doc.materials[0];
    CHECK(plain.base_color_factor == tg::vec4f(1, 1, 1, 1));
    CHECK(plain.metallic_factor == 1);
    CHECK(plain.roughness_factor == 1);
    CHECK(plain.alpha == babel::gltf::alpha_mode::opaque);
    CHECK(plain.alpha_cutoff == 0.5f);
    CHECK(!plain.double_sided);
    CHECK(plain.base_color_texture.texture == babel::gltf::texture_index::invalid);
    CHECK(plain.normal_scale == 1);
    CHECK(plain.occlusion_strength == 1);

    auto const& full = doc.materials[1];
    CHECK(full.base_color_factor == tg::vec4f(0.1f, 0.2f, 0.3f, 0.4f));
    CHECK(full.metallic_factor == 0.25f);
    CHECK(full.roughness_factor == 0.75f);
    CHECK(full.base_color_texture.texture == babel::gltf::texture_index(0));
    CHECK(full.base_color_texture.texcoord == 1);
    CHECK(full.normal_scale == 2);
    CHECK(full.occlusion_strength == 0.5f);
    CHECK(full.emissive_factor == tg::vec3f(1, 0, 0));
    CHECK(full.alpha == babel::gltf::alpha_mode::mask);
    CHECK(full.alpha_cutoff == 0.25f);
    CHECK(full.double_sided);

    REQUIRE(doc.samplers.size() == 1);
    CHECK(doc.samplers[0].mag_filter == babel::gltf::filter::linear);
    CHECK(doc.samplers[0].min_filter == babel::gltf::filter::linear_mipmap_linear);
    CHECK(doc.samplers[0].wrap_s == babel::gltf::wrap_mode::clamp_to_edge);
    CHECK(doc.samplers[0].wrap_t == babel::gltf::wrap_mode::repeat); // the spec's default

    REQUIRE(doc.textures.size() == 1);
    CHECK(doc.find(doc.textures[0].source) == &doc.images[0]);
    CHECK(doc.find(doc.textures[0].sampler) == &doc.samplers[0]);

    REQUIRE(doc.images.size() == 2);
    CHECK(doc.images[0].resolved);
    CHECK(doc.images[0].mime_type == "image/png");
    CHECK(doc.images[0].data.size() == 8);
    // A bufferView-backed image is a zero-copy subview of the input, ready for babel::image::read.
    CHECK(doc.images[0].data.data() >= pinned.data());
    CHECK(doc.images[0].data.data() + 8 <= pinned.data() + pinned.size());
    CHECK(doc.images[0].data[0] == byte(0x10));

    CHECK(!doc.images[1].resolved);
    CHECK(doc.images[1].uri == "external.png");
    CHECK(doc.images[1].data.empty());

    // The one thing this file asked for and did not get is on the record, and nothing else is.
    CHECK(count_issues(doc, babel::gltf::issue_kind::unresolved) == 1);
    CHECK(doc.issues.size() == 1);
}

TEST("gltf - external uri resolution through read_options")
{
    auto const bin = triangle_bin();
    auto const json = triangle_json(R"({"uri": "data.bin", "byteLength": 44})");

    auto asked_for = cc::string();
    auto resolver = [&](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
    {
        asked_for = cc::string(uri);
        return cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(bin));
    };

    auto const doc = babel::gltf::read(cc::string_view(json), {.resolve_uri = resolver}).value();
    CHECK(asked_for == "data.bin");
    REQUIRE(doc.buffers.size() == 1);
    CHECK(doc.buffers[0].resolved);
    CHECK(doc.buffers[0].uri == "data.bin"); // the uri stays recorded even once resolved
    CHECK(doc.read_indices(doc.primitives_of(doc.meshes[0])[0]).value().size() == 3);
    CHECK(!doc.has_issues()); // a served URI leaves nothing to report

    // Without a resolver the uri is recorded and the bytes stay absent — reading them is the error.
    auto const unresolved = babel::gltf::read(cc::string_view(json)).value();
    REQUIRE(unresolved.buffers.size() == 1);
    CHECK(!unresolved.buffers[0].resolved);
    CHECK(unresolved.buffers[0].uri == "data.bin");
    CHECK(unresolved.buffers[0].data.empty());
    CHECK(unresolved.view_of(unresolved.accessors[1]).has_error());
    CHECK(unresolved.has_issue_of(babel::gltf::issue_kind::unresolved));

    // A resolver that fails fails the whole read.
    auto failing = [](cc::string_view) -> cc::result<cc::pinned_data<byte const>> { return cc::error("no such file"); };
    CHECK(babel::gltf::read(cc::string_view(json), {.resolve_uri = failing}).has_error());
}

TEST("gltf - index validation and find helpers")
{
    auto const bin = triangle_bin();
    auto const doc = babel::gltf::read(cc::string_view(triangle_json(base64_buffer_spec(bin)))).value();

    CHECK(doc.find(babel::gltf::buffer_index(0)) == &doc.buffers[0]);
    CHECK(doc.find(babel::gltf::buffer_index::invalid) == nullptr);
    CHECK(doc.find(babel::gltf::mesh_index(0)) == &doc.meshes[0]);
    CHECK(doc.find(doc.nodes[0].mesh) == &doc.meshes[0]);
    CHECK(doc.find(doc.default_scene) == &doc.scenes[0]);

    // The primitive states no material, so the reference resolves to nothing rather than to material 0.
    CHECK(doc.find(doc.primitives_of(doc.meshes[0])[0].material) == nullptr);
    CHECK(doc.find(babel::gltf::material_index(0)) == nullptr); // and there are no materials at all

    auto const out_of_range = R"({"asset": {"version": "2.0"},
        "buffers": [{"uri": "data:;base64,AAAAAA==", "byteLength": 4}],
        "bufferViews": [{"buffer": 7, "byteLength": 4}]})";
    CHECK(babel::gltf::read(cc::string_view(out_of_range)).has_error());

    auto const dangling_node = R"({"asset": {"version": "2.0"}, "nodes": [{"children": [4]}]})";
    CHECK(babel::gltf::read(cc::string_view(dangling_node)).has_error());

    auto const dangling_scene = R"({"asset": {"version": "2.0"}, "scenes": [{"nodes": [1]}], "nodes": [{}]})";
    CHECK(babel::gltf::read(cc::string_view(dangling_scene)).has_error());

    auto const dangling_texture = R"({"asset": {"version": "2.0"}, "textures": [{"source": 0}]})";
    CHECK(babel::gltf::read(cc::string_view(dangling_texture)).has_error());
}

TEST("gltf - errors")
{
    // container / document structure
    CHECK(babel::gltf::read(cc::string_view("")).has_error());
    CHECK(babel::gltf::read(cc::string_view("[]")).has_error());               // root must be an object
    CHECK(babel::gltf::read(cc::string_view("{}")).has_error());               // `asset` is required
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {}})")).has_error()); // `asset.version` is required
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "1.0"}})")).has_error());
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "two"}})")).has_error());

    // a required extension we do not implement must be refused, not silently ignored
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "extensionsRequired": ["KHR_draco_mesh_compression"]})"))
              .has_error());

    // sparse accessors would hand back the wrong data
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "accessors": [{"componentType": 5126, "count": 1, "type": "VEC3", "sparse": {"count": 1}}]})"))
              .has_error());

    // values that decide how bytes are read
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "accessors": [{"componentType": 1234, "count": 1, "type": "VEC3"}]})"))
              .has_error());
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "accessors": [{"componentType": 5126, "count": 1, "type": "VEC7"}]})"))
              .has_error());
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "accessors": [{"count": 1, "type": "VEC3"}]})"))
              .has_error()); // componentType is required
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "meshes": [{"primitives": [{"attributes": {}, "mode": 9}]}]})"))
              .has_error());

    // an accessor that does not fit its bufferView
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "buffers": [{"uri": "data:;base64,AAAAAAAAAAA=", "byteLength": 8}],
        "bufferViews": [{"buffer": 0, "byteLength": 8}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}]})"))
              .has_error());

    // a bufferView that does not fit its buffer
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "buffers": [{"uri": "data:;base64,AAAAAA==", "byteLength": 4}],
        "bufferViews": [{"buffer": 0, "byteOffset": 2, "byteLength": 4}]})"))
              .has_error());

    // a buffer with no uri and no BIN chunk to take it from
    CHECK(
        babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"}, "buffers": [{"byteLength": 4}]})")).has_error());

    // data URIs must be base64
    CHECK(babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0"},
        "buffers": [{"uri": "data:text/plain,hello", "byteLength": 5}]})"))
              .has_error());

    // GLB container failures
    auto const truncated = glb_header(2, 12);
    CHECK(babel::gltf::read(cc::span<byte const>(truncated).first_n(4)).has_error()); // magic without a header
    CHECK(babel::gltf::read(cc::span<byte const>(truncated)).has_error());            // a header with no JSON chunk
    CHECK(babel::gltf::read(cc::span<byte const>(glb_header(1, 12))).has_error());    // version 1
    CHECK(babel::gltf::read(cc::span<byte const>(glb_header(2, 4096))).has_error());  // declared length past the input

    auto bin_first = glb_header(2, 12 + 8 + 4);
    push_le_u32(bin_first, 4);
    push_le_u32(bin_first, glb_chunk_bin);
    push_le_u32(bin_first, 0);
    CHECK(babel::gltf::read(cc::span<byte const>(bin_first)).has_error()); // the JSON chunk must come first

    auto overlong_chunk = glb_header(2, 12 + 8 + 4);
    push_le_u32(overlong_chunk, 64);
    push_le_u32(overlong_chunk, glb_chunk_json);
    push_le_u32(overlong_chunk, 0);
    CHECK(babel::gltf::read(cc::span<byte const>(overlong_chunk)).has_error()); // a chunk past the declared length
}

TEST("gltf - tolerated junk and skipped features")
{
    // Unknown members, extras / extensions, morph targets, and the arrays this reader does not model yet
    // must all pass through without complaint — none of them changes how a byte is interpreted.
    auto const doc = babel::gltf::read(cc::string_view(R"({"asset": {"version": "2.0", "extras": {"tool": 1}},
        "extensionsUsed": ["KHR_materials_unlit"],
        "extensions": {"KHR_materials_unlit": {}},
        "someExporterField": [1, 2, 3],
        "meshes": [{"primitives": [{"attributes": {}, "targets": [{"POSITION": 0}]}], "weights": [0.5]}],
        "nodes": [{"skin": 0, "camera": 0, "weights": [1]}],
        "skins": [{"joints": [0]}],
        "animations": [{"channels": [], "samplers": []}],
        "cameras": [{"type": "perspective"}],
        "materials": [{"metallicFactor": "not a number", "alphaMode": "WHATEVER",
                       "pbrMetallicRoughness": {"roughnessFactor": [1, 2]}}],
        "samplers": [{"magFilter": 1234, "wrapS": 4321}]})"))
                         .value();

    CHECK(doc.extensions_used.size() == 1);
    CHECK(doc.extensions_used[0] == "KHR_materials_unlit");
    CHECK(doc.extensions_required.empty());

    REQUIRE(doc.meshes.size() == 1);
    CHECK(doc.primitives_of(doc.meshes[0]).size() == 1);
    CHECK(doc.attributes_of(doc.primitives_of(doc.meshes[0])[0]).empty());
    CHECK(doc.nodes.size() == 1);

    // A wrong JSON type on an optional scalar falls back to the default rather than failing the read.
    REQUIRE(doc.materials.size() == 1);
    CHECK(doc.materials[0].metallic_factor == 1);
    CHECK(doc.materials[0].roughness_factor == 1);
    CHECK(doc.materials[0].alpha == babel::gltf::alpha_mode::opaque);

    // Unknown sampler enums are cosmetic, so they map to the neutral value.
    REQUIRE(doc.samplers.size() == 1);
    CHECK(doc.samplers[0].mag_filter == babel::gltf::filter::none);
    CHECK(doc.samplers[0].wrap_s == babel::gltf::wrap_mode::repeat);

    // Tolerated is not silent: every skip and every fallback above is on the record.
    CHECK(doc.has_issues());
    // the used extension, plus skins / animations / cameras / morph targets
    CHECK(count_issues(doc, babel::gltf::issue_kind::unsupported) == 5);
    // the two unknown sampler enumerants and the unknown alphaMode
    CHECK(count_issues(doc, babel::gltf::issue_kind::malformed) == 3);
    CHECK(count_issues(doc, babel::gltf::issue_kind::unresolved) == 0);
    CHECK(!doc.has_issue_of(babel::gltf::issue_kind::unresolved));
}

TEST("gltf - unresolved references are recorded, not errors")
{
    auto json = cc::string();
    json += R"({"asset": {"version": "2.0"},
        "buffers": [{"uri": "geometry.bin", "byteLength": 12}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 12}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 1, "type": "VEC3", "min": [0, 0, 0]}],
        "images": [{"uri": "tex.png"}, {"bufferView": 0, "mimeType": "image/png"}]})";

    auto const doc = babel::gltf::read(cc::string_view(json)).value();

    // Three references nobody could follow: the buffer, the URI image, and the image inside that buffer.
    CHECK(count_issues(doc, babel::gltf::issue_kind::unresolved) == 3);
    CHECK(doc.has_issue_of(babel::gltf::issue_kind::unresolved));
    CHECK(!doc.buffers[0].resolved);
    CHECK(!doc.images[0].resolved);
    CHECK(!doc.images[1].resolved);

    // `min` without `max` is not a bound, and dropping it is worth saying.
    CHECK(count_issues(doc, babel::gltf::issue_kind::malformed) == 1);
    CHECK(doc.min_of(doc.accessors[0]).empty());
    CHECK(doc.max_of(doc.accessors[0]).empty());

    // The report is one line per issue, each tagged with its kind.
    auto const report = doc.issue_report();
    CHECK(report.contains("unresolved: "));
    CHECK(report.contains("geometry.bin"));
    CHECK(report.contains("malformed: "));

    // Supplying the resolver clears the buffer and the bufferView-backed image; the URI image still needs its own fetch.
    auto payload = cc::vector<byte>();
    for (auto v = 0; v < 12; ++v)
        payload.push_back(byte(u8(v)));
    auto resolver = [&](cc::string_view uri) -> cc::result<cc::pinned_data<byte const>>
    {
        if (uri != "geometry.bin")
            return cc::error("only the geometry is served here");
        return cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(payload));
    };

    CHECK(babel::gltf::read(cc::string_view(json), {.resolve_uri = resolver}).has_error()); // the image resolver said no
}

TEST("gltf - glb records unpadded chunks and unknown chunk types")
{
    auto const json = cc::string_view(R"({"asset": {"version": "2.0"}})"); // 29 bytes, deliberately unpadded

    auto glb = cc::vector<byte>();
    push_le_u32(glb, glb_magic);
    push_le_u32(glb, 2);
    push_le_u32(glb, u32(12 + 8 + json.size() + 8 + 4));
    push_le_u32(glb, u32(json.size()));
    push_le_u32(glb, glb_chunk_json);
    push_bytes(glb, json.as_bytes());
    push_le_u32(glb, 4);
    push_le_u32(glb, 0x12345678); // an extension chunk we know nothing about; the spec says skip it
    push_le_u32(glb, 0);

    auto const doc = babel::gltf::read(cc::span<byte const>(glb)).value();

    CHECK(doc.source == babel::gltf::container::glb);
    CHECK(doc.asset.version == "2.0");
    CHECK(count_issues(doc, babel::gltf::issue_kind::malformed) == 1);   // the unpadded JSON chunk
    CHECK(count_issues(doc, babel::gltf::issue_kind::unsupported) == 1); // the skipped chunk
}

TEST("gltf - detect_container")
{
    auto const glb = make_glb(R"({"asset": {"version": "2.0"}})", cc::span<byte const>());
    CHECK(babel::gltf::detect_container(glb) == babel::gltf::container::glb);

    // Anything that does not open with the GLB magic is read as JSON, so garbage reports a JSON error.
    CHECK(babel::gltf::detect_container(cc::string_view("{}").as_bytes()) == babel::gltf::container::gltf);
    CHECK(babel::gltf::detect_container(cc::span<byte const>()) == babel::gltf::container::gltf);
}
