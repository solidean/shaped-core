#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>

using namespace cc::primitive_defines;

namespace
{
/// Compiles `desc`; the node comes back already finished, since nothing is compiled.
sg::async_compiled_shader compile(slib::shader_source_description const& desc)
{
    auto const compiler = slib::create_wgsl_compiler();
    auto shader = compiler->compile(desc);
    REQUIRE(shader != nullptr);
    REQUIRE((shader->has_value() || shader->has_error()));
    return shader;
}

cc::string error_of(slib::shader_source_description const& desc)
{
    auto const shader = compile(desc);
    REQUIRE(shader->has_error());
    return shader->try_error()->underlying().to_string();
}

constexpr auto k_compute_source = cc::string_view(R"(
    @group(0) @binding(0) var<storage, read_write> values: array<f32>;
    @compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3u) { values[id.x] *= 2.0; }
)");
} // namespace

TEST("slib wgsl compiler - advertises its wgsl -> wgsl edge")
{
    auto const compiler = slib::create_wgsl_compiler();
    CHECK(compiler->source_language() == slib::shader_language::wgsl);
    CHECK(compiler->target_format() == sg::shader_format::wgsl);
}

TEST("slib wgsl compiler - the source is the bytecode, and the declarations its reflection")
{
    auto const shader = compile({.source = k_compute_source, .entry_point = "main", .stage = sg::shader_stage::compute});
    REQUIRE(shader->has_value());
    auto const& s = *shader->try_value();
    CHECK(s.stage == sg::shader_stage::compute);
    CHECK(s.format == sg::shader_format::wgsl);
    CHECK(s.entry_point == "main");
    CHECK(s.bytecode.size() == k_compute_source.size());
    CHECK(char(s.bytecode[0]) == k_compute_source[0]);
    REQUIRE(s.workgroup_size.has_value());
    CHECK(s.workgroup_size.value().x == 64);
    REQUIRE(s.bindings.size() == 1);
    CHECK(s.bindings[0].type == sg::binding_type::readwrite_structured_buffer);
}

TEST("slib wgsl compiler - a module whose stage is not the declared one is an error")
{
    auto const e = error_of({.source = k_compute_source, .entry_point = "main", .stage = sg::shader_stage::fragment});
    CHECK(e.contains("'main' is not the stage the package declares"));
}

TEST("slib wgsl compiler - a module whose entry point is not the declared one is an error")
{
    auto const e = error_of({.source = k_compute_source, .entry_point = "cs_main", .stage = sg::shader_stage::compute});
    CHECK(e.contains("entry point is 'main', not 'cs_main'"));
}

TEST("slib wgsl compiler - a module that fails to reflect is an error")
{
    auto const e = error_of({.source = "fn helper() {}", .entry_point = "main", .stage = sg::shader_stage::compute});
    CHECK(e.contains("declares no entry point"));
}
