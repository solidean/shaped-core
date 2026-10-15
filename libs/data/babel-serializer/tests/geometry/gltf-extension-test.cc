#include <babel-data/data/base64.hh>
#include <babel-serializer/geometry/gltf.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// A caller's glTF extension, registered through read_options::extensions: its hooks decode bytes the reader then
// serves through view_of, as it serves the reader's own EXT_meshopt_compression.

namespace
{
namespace gltf = babel::gltf;

/// One float per element, written as `value` four times: what VENDOR_constant asks of an accessor with no bufferView.
gltf::decoded_bytes constant_accessor(gltf::accessor_decode const& c)
{
    auto const& a = c.doc.accessors[int(c.accessor)];
    auto const value = f32(c.extension["value"].as_double());
    auto out = cc::pinned_data<byte>::create_uninitialized(a.count * a.element_size());
    auto const* const v = reinterpret_cast<byte const*>(&value);
    for (auto i = isize(0); i < out.size(); ++i)
        out[i] = v[i % 4];
    return cc::optional<cc::pinned_data<byte const>>(out.as_bytes());
}

/// A bufferView whose buffer bytes are XORed with `key`: VENDOR_xor's decode.
gltf::decoded_bytes xor_view(gltf::buffer_view_decode const& c)
{
    auto const& view = c.doc.buffer_views[int(c.view)];
    auto const& buffer = c.doc.buffers[int(view.buffer)];
    if (!buffer.resolved)
        return cc::optional<cc::pinned_data<byte const>>();
    auto const key = u8(c.extension["key"].as_double());
    auto out = cc::pinned_data<byte>::create_uninitialized(view.byte_length);
    for (auto i = isize(0); i < out.size(); ++i)
        out[i] = byte(u8(buffer.data[view.byte_offset + i]) ^ key);
    return cc::optional<cc::pinned_data<byte const>>(out.as_bytes());
}

gltf::decoded_bytes never_decodes(gltf::buffer_view_decode const&)
{
    return cc::optional<cc::pinned_data<byte const>>();
}
gltf::decoded_bytes too_short(gltf::buffer_view_decode const&)
{
    return cc::optional<cc::pinned_data<byte const>>(cc::pinned_data<byte>::create_filled(3, byte(0)).as_bytes());
}
gltf::decoded_bytes fails(gltf::buffer_view_decode const&)
{
    return cc::error("the vendor's data is corrupt");
}

/// Three u32 XORed with 0x5A, in a data uri: the bytes VENDOR_xor's view decodes to 7, 8, 9.
cc::string xor_document(cc::string_view view_extensions)
{
    auto plain = cc::vector<byte>();
    for (auto const v : {7u, 8u, 9u})
        for (auto b = 0; b < 4; ++b)
            plain.push_back(byte(u8(v >> (8 * b)) ^ 0x5A));
    return cc::format(R"({{"asset": {{"version": "2.0"}}, "extensionsUsed": ["VENDOR_xor"],
        "buffers": [{{"uri": "data:application/octet-stream;base64,{}", "byteLength": 12}}],
        "bufferViews": [{{"buffer": 0, "byteLength": 12, "extensions": {}}}],
        "accessors": [{{"bufferView": 0, "componentType": 5125, "count": 3, "type": "SCALAR"}}]}})",
                      babel::base64::encode(plain), view_extensions);
}
} // namespace

TEST("gltf extension - an accessor hook stands in for a bufferView, and view_of reads what it decoded")
{
    auto const json = cc::string_view(R"({"asset": {"version": "2.0"},
        "extensionsUsed": ["VENDOR_constant"], "extensionsRequired": ["VENDOR_constant"],
        "accessors": [{"componentType": 5126, "count": 4, "type": "VEC2", "extensions": {"VENDOR_constant": {"value": 2.5}}},
                      {"componentType": 5126, "count": 2, "type": "SCALAR"}]})");
    auto hook = &constant_accessor;
    gltf::extension const extensions[] = {{.name = "VENDOR_constant", .decode_accessor = hook}};
    auto const doc = REQUIRED_VALUE(gltf::read(json, {.extensions = extensions}));
    CHECK(!doc.has_issues()).dump("issues", doc.issue_report());

    CHECK(doc.accessors[0].decoded_by == "VENDOR_constant");
    auto const values = REQUIRED_VALUE(REQUIRED_VALUE(doc.view_of(gltf::accessor_index(0))).read_elements<tg::vec2f>());
    REQUIRE(values.size() == 4);
    for (auto const& v : values)
        CHECK(v == tg::vec2f(2.5f, 2.5f));

    // an accessor without the extension is untouched: no bufferView is still the spec's implicit zeros, unread
    CHECK(doc.accessors[1].decoded_by.empty());
    CHECK(doc.view_of(gltf::accessor_index(1)).has_error());
}

TEST("gltf extension - a buffer-view hook decodes the view, and its accessors read through it")
{
    auto hook = &xor_view;
    gltf::extension const extensions[] = {{.name = "VENDOR_xor", .decode_buffer_view = hook}};
    auto const doc = REQUIRED_VALUE(
        gltf::read(cc::string_view(xor_document(R"({"VENDOR_xor": {"key": 90}})")), {.extensions = extensions}));
    CHECK(doc.buffer_views[0].decoded_by == "VENDOR_xor");
    auto const values = REQUIRED_VALUE(REQUIRED_VALUE(doc.view_of(gltf::accessor_index(0))).read_elements<u32>());
    REQUIRE(values.size() == 3);
    CHECK(values[0] == 7);
    CHECK(values[1] == 8);
    CHECK(values[2] == 9);
}

TEST("gltf extension - a hook that leaves an element undecoded is an issue, and view_of refuses the element")
{
    auto hook = &never_decodes;
    gltf::extension const extensions[] = {{.name = "VENDOR_xor", .decode_buffer_view = hook}};
    auto const doc = REQUIRED_VALUE(
        gltf::read(cc::string_view(xor_document(R"({"VENDOR_xor": {"key": 90}})")), {.extensions = extensions}));
    CHECK(doc.has_issue_of(gltf::issue_kind::unresolved));
    CHECK(!doc.buffer_views[0].decoded.has_value());
    auto const view = doc.view_of(gltf::accessor_index(0));
    REQUIRE(view.has_error());
    CHECK(view.error().to_string().contains("left it undecoded"));
}

TEST("gltf extension - a hook's error, or bytes of the wrong size, fail the read")
{
    auto const json = xor_document(R"({"VENDOR_xor": {"key": 90}})");

    auto failing = &fails;
    gltf::extension const failing_ext[] = {{.name = "VENDOR_xor", .decode_buffer_view = failing}};
    auto const r1 = gltf::read(cc::string_view(json), {.extensions = failing_ext});
    REQUIRE(r1.has_error());
    CHECK(r1.error().to_string().contains("the vendor's data is corrupt"));

    auto short_hook = &too_short;
    gltf::extension const short_ext[] = {{.name = "VENDOR_xor", .decode_buffer_view = short_hook}};
    auto const r2 = gltf::read(cc::string_view(json), {.extensions = short_ext});
    REQUIRE(r2.has_error());
    CHECK(r2.error().to_string().contains("decodes to 3 bytes"));
}

TEST("gltf extension - one element decoded by two extensions fails the read")
{
    auto hook = &xor_view;
    gltf::extension const extensions[]
        = {{.name = "VENDOR_xor", .decode_buffer_view = hook}, {.name = "VENDOR_other", .decode_buffer_view = hook}};
    auto const json = xor_document(R"({"VENDOR_xor": {"key": 90}, "VENDOR_other": {"key": 1}})");
    auto const r = gltf::read(cc::string_view(json), {.extensions = extensions});
    REQUIRE(r.has_error());
    CHECK(r.error().to_string().contains("decoded by both 'VENDOR_xor' and 'VENDOR_other'"));
}

TEST("gltf extension - a registration must name each extension once, and none the reader implements")
{
    auto const json = cc::string_view(R"({"asset": {"version": "2.0"}})");

    gltf::extension const twice[] = {{.name = "VENDOR_a"}, {.name = "VENDOR_a"}};
    auto const r1 = gltf::read(json, {.extensions = twice});
    REQUIRE(r1.has_error());
    CHECK(r1.error().to_string().contains("registered twice"));

    gltf::extension const builtin[] = {{.name = "EXT_meshopt_compression"}};
    auto const r2 = gltf::read(json, {.extensions = builtin});
    REQUIRE(r2.has_error());
    CHECK(r2.error().to_string().contains("implemented by this reader"));

    gltf::extension const unnamed[] = {{}};
    CHECK(gltf::read(json, {.extensions = unnamed}).has_error());
}
