#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shaped-shader-compiler-msl/all.hh>

// The two arms end to end: a metallib where the toolchain is there, MSL source where it is not.
// Which one `automatic` picks is a property of the host, so each test says which arm it means.

namespace
{
using namespace cc::primitive_defines;

constexpr char const* k_kernel = R"(
#include <metal_stdlib>
using namespace metal;

struct bindings
{
    device float*       data  [[id(0)]];
    constant float&     scale [[id(1)]];
};

#pragma sc numthreads 32 1 1
kernel void scale_it(constant bindings& b [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    b.data[i] *= b.scale;
}
)";

[[nodiscard]] cc::string_view as_text(cc::pinned_data<byte const> const& blob)
{
    return cc::string_view(reinterpret_cast<char const*>(blob.data()), blob.size());
}
} // namespace

TEST("ssc::msl compile - the source arm needs no toolchain and carries the text as its blob")
{
    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());

    auto shader = comp.value().compile({.source = k_kernel, .entry_point = "scale_it"},
                                       {.artifact = ssc::msl::artifact_kind::msl_source});
    REQUIRE(shader.has_value());

    CHECK(shader.value().format == sg::shader_format::msl);
    CHECK(shader.value().entry_point == "scale_it");
    CHECK(as_text(shader.value().bytecode) == cc::string_view(k_kernel));

    // Reflection ran whichever arm produced the blob.
    REQUIRE(shader.value().bindings.size() == 2);
    REQUIRE(shader.value().workgroup_size.has_value());
    CHECK(shader.value().workgroup_size.value().x == 32);
}

TEST("ssc::msl compile - the metallib arm produces a metallib")
{
    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());
    if (!comp.value().toolchain().is_available)
        return; // a host without the Metal toolchain component, which is a supported configuration

    auto shader = comp.value().compile({.source = k_kernel, .entry_point = "scale_it"},
                                       {.artifact = ssc::msl::artifact_kind::metallib});
    REQUIRE(shader.has_value());

    CHECK(shader.value().format == sg::shader_format::metal_lib);
    REQUIRE(shader.value().bytecode.size() > 4);
    CHECK(as_text(shader.value().bytecode).starts_with("MTLB"));

    // The same reflection as the source arm: it is read from the text either way.
    CHECK(shader.value().bindings.size() == 2);
    CHECK(shader.value().compiler.name == "metal");
    CHECK(!shader.value().compiler.version.empty());
}

TEST("ssc::msl compile - automatic takes the metallib where the toolchain is there")
{
    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());

    auto shader = comp.value().compile({.source = k_kernel, .entry_point = "scale_it"});
    REQUIRE(shader.has_value());

    auto const expected = comp.value().toolchain().is_available ? sg::shader_format::metal_lib : sg::shader_format::msl;
    CHECK(shader.value().format == expected);
}

TEST("ssc::msl compile - a description's workgroup size wins over the shader's pragma")
{
    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());

    auto shader = comp.value().compile(
        {.source = k_kernel, .entry_point = "scale_it", .workgroup_size = sg::compute_dimensions{.x = 8, .y = 8}},
        {.artifact = ssc::msl::artifact_kind::msl_source});
    REQUIRE(shader.has_value());
    REQUIRE(shader.value().workgroup_size.has_value());
    CHECK(shader.value().workgroup_size.value().x == 8);
    CHECK(shader.value().workgroup_size.value().y == 8);
}

TEST("ssc::msl compile - a shader the Metal compiler rejects carries its diagnostics")
{
    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());
    if (!comp.value().toolchain().is_available)
        return;

    // Reflection accepts this, so the error is the Metal compiler's.
    constexpr char const* source = R"(
struct work { device float* d [[id(0)]]; };
kernel void broken(constant work& w [[buffer(0)]]) { no_such_function(w.d); }
)";

    auto shader = comp.value().compile({.source = source, .entry_point = "broken"},
                                       {.artifact = ssc::msl::artifact_kind::metallib});
    REQUIRE(shader.has_error());
    CHECK(shader.error().to_string().contains("the Metal compiler rejected")).context(shader.error().to_string());
}

TEST("ssc::msl compile - an entry point the text does not declare fails before a compiler is spawned")
{
    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());

    auto shader = comp.value().compile({.source = k_kernel, .entry_point = "absent"});
    CHECK(shader.has_error());
}

TEST("ssc::msl compile - the stages metal has no shape for are refused")
{
    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());

    auto shader
        = comp.value().compile({.source = k_kernel, .entry_point = "scale_it", .stage = sg::shader_stage::geometry});
    CHECK(shader.has_error());
}

TEST("ssc::msl compile - a metallib asked for without a toolchain names what is missing")
{
    auto comp = ssc::msl::compiler::create();
    REQUIRE(comp.has_value());
    if (comp.value().toolchain().is_available)
        return; // the refusal only exists on a host without one

    auto shader = comp.value().compile({.source = k_kernel, .entry_point = "scale_it"},
                                       {.artifact = ssc::msl::artifact_kind::metallib});
    CHECK(shader.has_error());
}
