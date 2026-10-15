#include <babel-data/data/base64.hh>
#include <babel-serializer/geometry/gltf.hh>
#include <clean-core/common/endian.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// EXT_meshopt_compression: a bufferView decoded during read, and read through view_of like any other.
// The packed fixtures were written by meshoptimizer's own encoder at the vendored pin (extern/meshoptimizer), each
// next to the bytes it was encoded from; the decoder is all babel carries.

namespace
{
namespace gltf = babel::gltf;

bool equals(cc::vector<u32> const& got, std::initializer_list<u32> want)
{
    if (got.size() != isize(want.size()))
        return false;
    auto i = isize(0);
    for (auto const w : want)
        if (got[i++] != w)
            return false;
    return true;
}

template <class... Bytes>
cc::vector<byte> bytes_of(Bytes... b)
{
    return cc::vector<byte>{byte(b)...};
}

// 4 vertices of 16 bytes: a float3 position and a u32 tag of 1, 2, 3, 4
auto const vertices_packed = bytes_of(0xa1,
                                      0xfa,
                                      0xea,
                                      0xaa,
                                      0xab,
                                      0x00,
                                      0x00,
                                      0xff,
                                      0x00,
                                      0x00,
                                      0x7f,
                                      0x7e,
                                      0x7f,
                                      0x00,
                                      0x00,
                                      0x7f,
                                      0x00,
                                      0x00,
                                      0x02,
                                      0x02,
                                      0x02,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x01,
                                      0x00,
                                      0x00,
                                      0x00,
                                      0x01,
                                      0x01,
                                      0x00,
                                      0x00);

// the triangles 0 1 2 and 2 1 3
auto const triangles_packed
    = bytes_of(0xe1, 0xf0, 0x10, 0x00, 0x76, 0x87, 0x56, 0x67, 0x78, 0xa9, 0x86, 0x65, 0x89, 0x68, 0x98, 0x01, 0x69, 0x00, 0x00);

// the index sequence 7 3 9 4 4
auto const sequence_packed = bytes_of(0xd1, 0x1c, 0x0e, 0x18, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00);

// the normals +z and +x, octahedral in 8 bits a component
auto const oct_packed = bytes_of(0xa1,
                                 0xab,
                                 0x00,
                                 0xfe,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x7f,
                                 0x00,
                                 0x00);

// the floats (1.5, -2.25, 1000) and (0.125, 3, -7.5), exponential
auto const exp_packed = bytes_of(0xa1,
                                 0xfa,
                                 0xba,
                                 0xfa,
                                 0x00,
                                 0x3f,
                                 0x00,
                                 0x05,
                                 0x00,
                                 0xaf,
                                 0x00,
                                 0x16,
                                 0x00,
                                 0x0d,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x60,
                                 0xea,
                                 0x00,
                                 0x00,
                                 0xb8,
                                 0xeb,
                                 0x00,
                                 0x00,
                                 0x7d,
                                 0xf3,
                                 0x00,
                                 0x00,
                                 0x00);

/// One packed view: where its bytes sit in the packed buffer, and what the extension says of them.
struct packed_view
{
    cc::vector<byte> const* packed = nullptr;
    i64 stride = 0;
    i64 count = 0;
    char const* mode = "ATTRIBUTES";
    char const* filter = "NONE";
};

/// A document whose bufferView i is `views[i]`, packed in buffer 0 (a data URI) over the fallback buffer 1.
cc::string meshopt_document(cc::span<packed_view const> views, cc::string_view accessors_and_meshes)
{
    auto packed = cc::vector<byte>();
    auto view_json = cc::string();
    auto fallback_offset = i64(0);
    for (auto i = isize(0); i < views.size(); ++i)
    {
        auto const& v = views[i];
        auto const at = packed.size();
        for (auto const b : *v.packed)
            packed.push_back(b);
        auto const decoded = v.stride * v.count;
        view_json += cc::format(
            R"({}{{"buffer": 1, "byteOffset": {}, "byteLength": {}{}, )"
            R"("extensions": {{"EXT_meshopt_compression": {{"buffer": 0, "byteOffset": {}, "byteLength": {}, )"
            R"("byteStride": {}, "count": {}, "mode": "{}", "filter": "{}"}}}}}})",
            i == 0 ? "" : ", ", fallback_offset, decoded,
            cc::string_view(v.mode) == "ATTRIBUTES" ? cc::format(R"(, "byteStride": {})", v.stride) : cc::string(), at,
            v.packed->size(), v.stride, v.count, v.mode, v.filter);
        fallback_offset += decoded;
    }

    auto json = cc::string(
        R"({"asset": {"version": "2.0"}, )"
        R"("extensionsUsed": ["EXT_meshopt_compression"], "extensionsRequired": ["EXT_meshopt_compression"], )");
    json += cc::format(R"("buffers": [{{"uri": "data:application/octet-stream;base64,{}", "byteLength": {}}}, )",
                       babel::base64::encode(packed), packed.size());
    json += cc::format(R"({{"byteLength": {}, "extensions": {{"EXT_meshopt_compression": {{"fallback": true}}}}}}], )",
                       fallback_offset);
    json += cc::format(R"("bufferViews": [{}])", view_json);
    if (!accessors_and_meshes.empty())
        json += cc::format(", {}", accessors_and_meshes);
    json += "}";
    return json;
}
} // namespace

TEST("gltf meshopt - a packed triangle mesh reads through view_of as if it were plain")
{
    packed_view const views[] = {
        {.packed = &vertices_packed, .stride = 16, .count = 4},
        {.packed = &triangles_packed, .stride = 2, .count = 6, .mode = "TRIANGLES"},
    };
    auto const json
        = meshopt_document(views, R"("accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
                                {"bufferView": 0, "byteOffset": 12, "componentType": 5125, "count": 4, "type": "SCALAR"},
                                {"bufferView": 1, "componentType": 5123, "count": 6, "type": "SCALAR"}],
               "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "_TAG": 1}, "indices": 2}]}])");

    auto const doc = REQUIRED_VALUE(gltf::read(cc::string_view(json)));
    CHECK(!doc.has_issues()).dump("issues", doc.issue_report());
    REQUIRE(doc.buffers.size() == 2);
    CHECK(doc.buffers[1].is_fallback);
    CHECK(!doc.buffers[1].resolved);
    REQUIRE(doc.buffer_views[0].meshopt.has_value());
    CHECK(doc.buffer_views[0].meshopt.value().mode == gltf::meshopt_mode::attributes);
    CHECK(doc.buffer_views[1].meshopt.value().mode == gltf::meshopt_mode::triangles);

    auto const positions
        = REQUIRED_VALUE(REQUIRED_VALUE(doc.view_of(gltf::accessor_index(0))).read_elements<tg::vec3f>());
    REQUIRE(positions.size() == 4);
    CHECK(positions[0] == tg::vec3f(0, 0, 0));
    CHECK(positions[1] == tg::vec3f(1, 0, 0));
    CHECK(positions[2] == tg::vec3f(0, 1, 0));
    CHECK(positions[3] == tg::vec3f(1, 1, 0));

    // an accessor offset inside a decoded view
    auto const tags = REQUIRED_VALUE(REQUIRED_VALUE(doc.view_of(gltf::accessor_index(1))).read_elements<u32>());
    CHECK(equals(tags, {1, 2, 3, 4}));

    auto const indices = REQUIRED_VALUE(doc.read_indices(doc.primitives_of(doc.meshes[0])[0]));
    CHECK(equals(indices, {0, 1, 2, 2, 1, 3}));
}

TEST("gltf meshopt - an index sequence decodes to 32-bit indices")
{
    packed_view const views[] = {{.packed = &sequence_packed, .stride = 4, .count = 5, .mode = "INDICES"}};
    auto const json = meshopt_document(
        views, R"("accessors": [{"bufferView": 0, "componentType": 5125, "count": 5, "type": "SCALAR"}])");
    auto const doc = REQUIRED_VALUE(gltf::read(cc::string_view(json)));
    auto const seq = REQUIRED_VALUE(REQUIRED_VALUE(doc.view_of(gltf::accessor_index(0))).read_elements<u32>());
    CHECK(equals(seq, {7, 3, 9, 4, 4}));
}

TEST("gltf meshopt - the octahedral and exponential filters are undone")
{
    packed_view const views[] = {
        {.packed = &oct_packed, .stride = 4, .count = 2, .filter = "OCTAHEDRAL"},
        {.packed = &exp_packed, .stride = 12, .count = 2, .filter = "EXPONENTIAL"},
    };
    auto const json = meshopt_document(
        views, R"("accessors": [{"bufferView": 0, "componentType": 5120, "normalized": true, "count": 2, "type": "VEC4"},
                                {"bufferView": 1, "componentType": 5126, "count": 2, "type": "VEC3"}])");
    auto const doc = REQUIRED_VALUE(gltf::read(cc::string_view(json)));

    // a unit vector comes back as signed normalized components: +z, then +x
    auto const normals = REQUIRED_VALUE(doc.view_of(gltf::accessor_index(0)));
    auto const n0 = normals.element(0);
    auto const n1 = normals.element(1);
    CHECK(i8(n0[0]) == 0);
    CHECK(i8(n0[1]) == 0);
    CHECK(i8(n0[2]) == 127);
    CHECK(i8(n1[0]) == 127);
    CHECK(i8(n1[1]) == 0);
    CHECK(i8(n1[2]) == 0);

    // the values are exact in 24 bits of mantissa, so they come back exactly
    auto const floats = REQUIRED_VALUE(REQUIRED_VALUE(doc.view_of(gltf::accessor_index(1))).read_elements<tg::vec3f>());
    REQUIRE(floats.size() == 2);
    CHECK(floats[0] == tg::vec3f(1.5f, -2.25f, 1000.0f));
    CHECK(floats[1] == tg::vec3f(0.125f, 3.0f, -7.5f));
}

TEST("gltf meshopt - packed bytes that do not decode, or a broken extension, fail the read")
{
    auto corrupt = vertices_packed;
    corrupt[0] = byte(0x00); // the header byte that names the codec version
    packed_view const bad_bytes[] = {{.packed = &corrupt, .stride = 16, .count = 4}};
    auto const r1 = gltf::read(cc::string_view(meshopt_document(bad_bytes, "")));
    REQUIRE(r1.has_error());
    CHECK(r1.error().to_string().contains("do not decode")).dump("error", r1.error().to_string());

    packed_view const bad_mode[] = {{.packed = &vertices_packed, .stride = 16, .count = 4, .mode = "VERTICES"}};
    auto const r2 = gltf::read(cc::string_view(meshopt_document(bad_mode, "")));
    REQUIRE(r2.has_error());
    CHECK(r2.error().to_string().contains("mode 'VERTICES'")).dump("error", r2.error().to_string());

    packed_view const bad_stride[] = {{.packed = &vertices_packed, .stride = 6, .count = 4}};
    auto const r3 = gltf::read(cc::string_view(meshopt_document(bad_stride, "")));
    REQUIRE(r3.has_error());
    CHECK(r3.error().to_string().contains("multiple of 4")).dump("error", r3.error().to_string());

    packed_view const bad_filter[]
        = {{.packed = &triangles_packed, .stride = 2, .count = 6, .mode = "TRIANGLES", .filter = "OCTAHEDRAL"}};
    auto const r4 = gltf::read(cc::string_view(meshopt_document(bad_filter, "")));
    REQUIRE(r4.has_error());
    CHECK(r4.error().to_string().contains("attributes alone")).dump("error", r4.error().to_string());
}

TEST("gltf meshopt - a view whose packed buffer was never resolved is an issue, and view_of says why")
{
    auto const json = cc::string(
        R"({"asset": {"version": "2.0"},
            "buffers": [{"uri": "packed.bin", "byteLength": 45},
                        {"byteLength": 64, "extensions": {"EXT_meshopt_compression": {"fallback": true}}}],
            "bufferViews": [{"buffer": 1, "byteLength": 64, "byteStride": 16,
                             "extensions": {"EXT_meshopt_compression": {"buffer": 0, "byteLength": 45, "byteStride": 16,
                                                                        "count": 4, "mode": "ATTRIBUTES"}}}],
            "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"}]})");
    auto const doc = REQUIRED_VALUE(gltf::read(cc::string_view(json)));
    CHECK(doc.has_issue_of(gltf::issue_kind::unresolved));
    CHECK(doc.buffer_views[0].decoded.empty());
    auto const view = doc.view_of(gltf::accessor_index(0));
    REQUIRE(view.has_error());
    CHECK(view.error().to_string().contains("never decoded"));
}
