#include "fake_compiler.hh"

#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/shader_library.hh>

#include <memory>

// compile_source: the door for shader text that was never a file — generated, downloaded, or typed into a UI.
// Driven by the fake compiler, so what is checked here is the PATH — that a source under no package compiles at all, and that its includes still resolve against the mount table.
// That real HLSL comes out the other end is dxc_compiler-test's job.

using slib_test::fake_compiler;

namespace
{
std::shared_ptr<slib::memory_filesystem> make_includes()
{
    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("helpers.hlsli", "helper body\n");
    fs->write("nested/deep.hlsli", "deep body\n");
    return fs;
}

sg::compiled_shader const& await(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    REQUIRE(shader->is_ready());
    REQUIRE(shader->has_value());
    return *shader->try_value();
}
} // namespace

TEST("slib - compile_source builds text that belongs to no package", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));

    // No package is registered at all, which is exactly what compile_shader cannot serve.
    auto const shader = lib.compile_source("generated body", sg::shader_stage::compute, "main", sg::shader_format::dxil);

    auto const& compiled = await(shader);
    CHECK(compiled.entry_point == "main");
    CHECK(compiled.stage == sg::shader_stage::compute);
    CHECK(fake_compiler::source_of(compiled).contains("generated body"));
}

TEST("slib - a generated source includes the hand-authored files a mount holds", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
    lib.mount("shared", make_includes());

    // Reached through the mount root, since a generated source sits under no package to search first.
    auto const shader = lib.compile_source("#include \"shared/helpers.hlsli\"\ngenerated body",
                                           sg::shader_stage::compute, "main", sg::shader_format::dxil);

    auto const& compiled = await(shader);
    CHECK(fake_compiler::source_of(compiled).contains("helper body"));
    CHECK(fake_compiler::source_of(compiled).contains("generated body"));
}

TEST("slib - include_dir is where a generated source looks first", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
    lib.mount("shared", make_includes());

    // With the directory named, the generated text spells the include the way a file sitting there would.
    auto const shader = lib.compile_source("#include \"helpers.hlsli\"\ngenerated body", sg::shader_stage::compute,
                                           "main", sg::shader_format::dxil, {.include_dir = "shared"});

    CHECK(fake_compiler::source_of(await(shader)).contains("helper body"));
}

TEST("slib - a failing compile_source reports rather than throws", exclusive("slib-shader-library"))
{
    {
        // No compiler at all for the requested format.
        slib::shader_library lib;
        auto const shader = lib.compile_source("body", sg::shader_stage::compute, "main", sg::shader_format::dxil);
        REQUIRE(shader != nullptr);
        REQUIRE(shader->is_ready());
        CHECK(!shader->has_value());
    }

    {
        // An include nothing can resolve.
        slib::shader_library lib;
        lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
        auto const shader = lib.compile_source("#include \"missing.hlsli\"\nbody", sg::shader_stage::compute, "main",
                                               sg::shader_format::dxil);
        REQUIRE(shader != nullptr);
        REQUIRE(shader->is_ready());
        CHECK(!shader->has_value());
    }
}
