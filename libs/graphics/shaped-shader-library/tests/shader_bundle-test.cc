#include "fake_compiler.hh"

#include <clean-core/container/pinned_data.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/impl/shader_bundle.hh>
#include <shaped-shader-library/shader_bundle.hh>
#include <shaped-shader-library/shader_library.hh>

#include <memory>

using namespace cc::primitive_defines;

// A shader bundle: bytecode a recording run compiled, served by a later run without compiling.
// The registry is process-wide, so each test bundles a source no other test compiles.

using slib_test::fake_compiler;

namespace
{
sg::compiled_shader const& await(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    REQUIRE(shader->is_ready());
    REQUIRE(shader->has_value());
    return *shader->try_value();
}

sg::compiled_shader precompiled(cc::string_view bytecode)
{
    auto shader = sg::compiled_shader();
    shader.stage = sg::shader_stage::compute;
    shader.format = sg::shader_format::dxil;
    shader.entry_point = "main";
    shader.bytecode = cc::pinned_data<byte const>(cc::pinned_data<byte>::create_copy_of(bytecode.as_bytes()));
    shader.compiler = sg::compiler_info{.name = "recorded", .version = "1", .signature = "main"};
    return shader;
}
} // namespace

TEST("slib - a bundled shader is served without compiling it", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    auto compiler = std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil);
    auto const* const fake = compiler.get();
    lib.add_compiler(cc::move(compiler));

    // HLSL is keyed once flattened, and this text includes nothing, so its flattened form is itself.
    auto const key
        = slib::impl::bundle_key(slib::shader_language::hlsl, sg::shader_format::dxil, sg::shader_stage::compute,
                                 "main", {}, {}, "a body only the bundle test compiles", {});
    // Leaked on purpose: a registered bundle must outlive the process, as embedded data does.
    auto* const bytes = new cc::vector<byte>(slib::impl::make_bundle(key, precompiled("from the bundle")));
    slib::register_shader_bundle(*bytes);

    auto const node = lib.compile_source("a body only the bundle test compiles", sg::shader_stage::compute, "main",
                                         sg::shader_format::dxil);
    auto const& shader = await(node);
    CHECK(fake_compiler::source_of(shader) == "from the bundle");
    CHECK(shader.compiler.name == "recorded");
    CHECK(fake->compile_count() == 0);

    // Anything else still compiles as it always did.
    auto const other_node
        = lib.compile_source("a body no bundle holds", sg::shader_stage::compute, "main", sg::shader_format::dxil);
    auto const& other = await(other_node);
    CHECK(fake_compiler::source_of(other) == "a body no bundle holds");
    CHECK(fake->compile_count() == 1);
}

TEST("slib - a bundle key ignores carriage returns and the order modules are listed in")
{
    auto const a = slib::module_source{.path = "mods/a.sgl", .text = "module a\n"};
    auto const b = slib::module_source{.path = "mods/b.sgl", .text = "module b\n"};
    auto const b_crlf = slib::module_source{.path = "mods/b.sgl", .text = "module b\r\n"};

    auto const key = [](cc::string_view source, std::initializer_list<slib::module_source> modules)
    {
        return slib::impl::bundle_key(slib::shader_language::sgl, sg::shader_format::dxil, sg::shader_stage::compute,
                                      "main", {}, "ns", source,
                                      cc::span<slib::module_source const>(modules.begin(), modules.size()));
    };

    // A checkout may hold CRLF files while the embedded copy holds LF, and the two must find each other's shaders.
    CHECK(key("use a\nbody\n", {a, b}) == key("use a\r\nbody\r\n", {a, b_crlf}));
    // The disk and the embedded copy may list a module directory in different orders.
    CHECK(key("use a\nbody\n", {a, b}) == key("use a\nbody\n", {b, a}));

    // What a compile reads still separates two keys.
    CHECK(key("use a\nbody\n", {a, b}) != key("use a\nother\n", {a, b}));
    CHECK(key("use a\nbody\n", {a, b}) != key("use a\nbody\n", {a}));
}
