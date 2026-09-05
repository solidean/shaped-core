#include <clean-core/container/pinned_data.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/impl/shader_codec.hh>

using namespace cc::primitive_defines;

// The codec is a pure CPU value transform — no device, no backend.
// What it has to guarantee is narrow and total: what goes in comes back, and anything else comes back as nothing.

namespace
{
sg::compiled_shader make_shader()
{
    auto shader = sg::compiled_shader();
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = "main";
    shader.workgroup_size = sg::compute_dimensions{.x = 64, .y = 2, .z = 1};

    byte const code[] = {byte(0xDE), byte(0xAD), byte(0xBE), byte(0xEF), byte(0x00), byte(0x7F)};
    shader.bytecode = cc::make_pinned_data(cc::span<byte const>(code));

    shader.bindings.push_back(
        {.name = "Output", .index = 1, .count = 2, .type = sg::binding_type::readwrite_structured_buffer});
    shader.bindings.push_back(
        {.name = "Params", .space = 1, .index = 0, .count = 1, .type = sg::binding_type::uniform_buffer, .block_size = 64});
    shader.bindings.push_back({.name = "Albedo",
                               .group_index = 0,
                               .space = 0,
                               .index = 2,
                               .count = 1,
                               .type = sg::binding_type::readonly_texture,
                               .texture_dimension = sg::texture_view_dimension::cube_array});

    shader.compiler = {.name = "dxc", .version = "1.8", .signature = "-T cs_6_8 -E main"};
    return shader;
}

bool same(sg::compiled_shader const& a, sg::compiled_shader const& b)
{
    if (a.stage != b.stage || a.format != b.format || a.entry_point != b.entry_point)
        return false;
    if (a.bytecode.size() != b.bytecode.size())
        return false;
    for (auto i = isize(0); i < a.bytecode.size(); ++i)
        if (a.bytecode[i] != b.bytecode[i])
            return false;
    if (a.bindings.size() != b.bindings.size())
        return false;
    for (auto i = isize(0); i < a.bindings.size(); ++i)
    {
        auto const& x = a.bindings[i];
        auto const& y = b.bindings[i];
        if (x.name != y.name || x.group_index != y.group_index || x.space != y.space || x.index != y.index
            || x.count != y.count || x.type != y.type)
            return false;
        if (x.block_size.has_value() != y.block_size.has_value())
            return false;
        if (x.block_size.has_value() && x.block_size.value() != y.block_size.value())
            return false;
        if (x.texture_dimension != y.texture_dimension)
            return false;
    }
    if (a.workgroup_size.has_value() != b.workgroup_size.has_value())
        return false;
    if (a.workgroup_size.has_value())
    {
        auto const& x = a.workgroup_size.value();
        auto const& y = b.workgroup_size.value();
        if (x.x != y.x || x.y != y.y || x.z != y.z)
            return false;
    }
    return a.compiler.name == b.compiler.name && a.compiler.version == b.compiler.version
        && a.compiler.signature == b.compiler.signature;
}
} // namespace

TEST("sg shader codec round-trips every field")
{
    auto const original = make_shader();
    auto const encoded = sg::impl::encode_compiled_shader(original);
    REQUIRE(!encoded.empty());

    auto const decoded = sg::impl::decode_compiled_shader(encoded);
    REQUIRE(decoded.has_value());
    CHECK(same(original, decoded.value()));
}

TEST("sg shader codec round-trips the absent optionals")
{
    auto original = make_shader();
    original.workgroup_size = {};
    original.bindings.clear();
    original.compiler = {};
    original.entry_point = {};

    auto const decoded = sg::impl::decode_compiled_shader(sg::impl::encode_compiled_shader(original));
    REQUIRE(decoded.has_value());
    CHECK(same(original, decoded.value()));
    CHECK(!decoded.value().workgroup_size.has_value());
}

TEST("sg shader codec refuses anything it did not write")
{
    auto const encoded = sg::impl::encode_compiled_shader(make_shader());

    // Truncation at every length: a cache may miss, never lie, so none of these may come back as a shader.
    for (auto cut = isize(0); cut < encoded.size(); ++cut)
        CHECK(!sg::impl::decode_compiled_shader(cc::span<byte const>(encoded).first_n(cut)).has_value());

    // Trailing bytes mean this is not the blob we think it is.
    auto extended = encoded;
    extended.push_back(byte(0));
    CHECK(!sg::impl::decode_compiled_shader(extended).has_value());

    // A version this build does not know.
    auto wrong_version = encoded;
    wrong_version[0] = byte(0xFF);
    CHECK(!sg::impl::decode_compiled_shader(wrong_version).has_value());

    // An enum value out of range, rather than a silently invalid one.
    auto bad_stage = encoded;
    bad_stage[4] = byte(0x7F);
    CHECK(!sg::impl::decode_compiled_shader(bad_stage).has_value());

    CHECK(!sg::impl::decode_compiled_shader({}).has_value());
}

TEST("sg shader codec refuses a length larger than the blob holding it")
{
    // A length prefix is the one field a corrupt blob can use to make the decoder allocate, so those are what this
    // test aims at — flipping bytes anywhere else usually lands in content, which decodes perfectly well and proves
    // nothing.
    //
    // The offsets follow the layout: version, stage, format, then the entry-point string, then the bytecode run.
    // A layout change breaks this test, which is intended — it is the same change that must bump k_shader_codec_version.
    auto shader = make_shader();
    shader.entry_point = "main"; // 4 bytes, so the offsets below are fixed
    auto const encoded = sg::impl::encode_compiled_shader(shader);

    constexpr auto entry_point_length_offset = isize(4 + 4 + 4);
    constexpr auto bytecode_length_offset = entry_point_length_offset + 8 + 4;

    auto const with_huge_length_at = [&](isize offset)
    {
        auto corrupt = encoded;
        for (auto i = isize(0); i < 8; ++i)
            corrupt[offset + i] = byte(0xFF);
        return corrupt;
    };

    // Both must come back as nothing rather than as a reservation the input chose.
    CHECK(!sg::impl::decode_compiled_shader(with_huge_length_at(entry_point_length_offset)).has_value());
    CHECK(!sg::impl::decode_compiled_shader(with_huge_length_at(bytecode_length_offset)).has_value());
}
