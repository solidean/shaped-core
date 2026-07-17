#include "fake_compiler.hh"

#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-shader-library/shader_package.hh>

#include <memory>

// The package -> asset -> compile path, driven by a memory_filesystem and a fake compiler, so none of it
// needs a disk or a real shader compiler.

using slib_test::fake_compiler;

namespace
{
/// Stands in for what the generator emits: the globals a package's definitions point at.
struct test_package
{
    slib::shader_asset_handle invert;
    slib::shader_asset_handle blit_vs;
    slib::shader_asset_handle blit_ps;

    slib::shader_definition definitions[3] = {
        {.path = "invert.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &invert},
        {.path = "blit.hlsl", .stage = sg::shader_stage::vertex, .entry_point = "main_vs", .asset = &blit_vs},
        {.path = "blit.hlsl", .stage = sg::shader_stage::fragment, .entry_point = "main_ps", .asset = &blit_ps},
    };

    [[nodiscard]] slib::shader_package package() const
    {
        return slib::shader_package{.name = "test_pkg",
                                    .language = slib::shader_language::hlsl,
                                    .source_dir = {},
                                    .embedded_files = {},
                                    .definitions = definitions};
    }
};

std::shared_ptr<slib::memory_filesystem> make_sources()
{
    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("invert.hlsl", "invert body");
    fs->write("blit.hlsl", "blit body");
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

TEST("slib - add_package fills in the generated globals")
{
    slib::shader_library lib;
    test_package pkg;

    CHECK(pkg.invert == nullptr); // nothing before registration

    lib.add_package(pkg.package(), make_sources());

    REQUIRE(pkg.invert != nullptr);
    REQUIRE(pkg.blit_vs != nullptr);
    REQUIRE(pkg.blit_ps != nullptr);
    CHECK(lib.assets().size() == 3);

    // Each definition's identity survives onto its asset, and the path is package-qualified.
    CHECK(pkg.invert->virtual_path() == "test_pkg/invert.hlsl");
    CHECK(pkg.invert->stage() == sg::shader_stage::compute);
    CHECK(pkg.invert->entry_point() == "main");

    // Two entry points in one file are two assets over the same source.
    CHECK(pkg.blit_vs->virtual_path() == "test_pkg/blit.hlsl");
    CHECK(pkg.blit_ps->virtual_path() == "test_pkg/blit.hlsl");
    CHECK(pkg.blit_vs->stage() == sg::shader_stage::vertex);
    CHECK(pkg.blit_ps->stage() == sg::shader_stage::fragment);
}

TEST("slib - the same package cannot be added twice")
{
    slib::shader_library lib;
    test_package pkg;

    lib.add_package(pkg.package(), make_sources());
    CHECK_ASSERTS(lib.add_package(pkg.package(), make_sources()));
}

TEST("slib - only one library may exist at a time")
{
    slib::shader_library lib;
    // The generated symbols are process-wide globals; a second library would fight over them.
    CHECK_ASSERTS(slib::shader_library{});
}

TEST("slib - acquiring through a global whose library is gone reports an error")
{
    // The generated globals are statics, so an asset outliving its library is normal, not a misuse.
    // The asset only weakly references the library, which is what keeps this a reported error rather
    // than a dangling read.
    test_package pkg;
    {
        slib::shader_library lib;
        lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
        lib.add_package(pkg.package(), make_sources());
        CHECK(pkg.invert->acquire(sg::shader_format::dxil)->has_value());
    }

    REQUIRE(pkg.invert != nullptr); // the global still points at the asset
    CHECK(pkg.invert->acquire(sg::shader_format::dxil)->has_error());
}

TEST("slib - acquire compiles a shader lazily, once per format")
{
    slib::shader_library lib;
    auto compiler = std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil);
    auto const* const compiler_ptr = compiler.get();
    lib.add_compiler(cc::move(compiler));

    test_package pkg;
    lib.add_package(pkg.package(), make_sources());

    CHECK(compiler_ptr->compile_count() == 0); // registration compiles nothing

    auto const first = pkg.invert->acquire(sg::shader_format::dxil);
    CHECK(compiler_ptr->compile_count() == 1);
    CHECK(fake_compiler::source_of(await(first)) == "invert body");
    CHECK(await(first).stage == sg::shader_stage::compute);
    CHECK(await(first).entry_point == "main");
    CHECK(await(first).format == sg::shader_format::dxil);

    // Acquiring again is free and hands back the very same node.
    auto const second = pkg.invert->acquire(sg::shader_format::dxil);
    CHECK(compiler_ptr->compile_count() == 1);
    CHECK(second == first);

    // A different shader is a separate compile.
    (void)pkg.blit_vs->acquire(sg::shader_format::dxil);
    CHECK(compiler_ptr->compile_count() == 2);
}

TEST("slib - acquire builds each requested format from the same source")
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::spirv));

    test_package pkg;
    lib.add_package(pkg.package(), make_sources());

    // One authored shader, two backend formats — the fan-out the compiler seam exists for.
    auto const dxil = pkg.invert->acquire(sg::shader_format::dxil);
    auto const spirv = pkg.invert->acquire(sg::shader_format::spirv);

    CHECK(await(dxil).format == sg::shader_format::dxil);
    CHECK(await(spirv).format == sg::shader_format::spirv);
    CHECK(dxil != spirv);
}

TEST("slib - can_compile reports the registered edges")
{
    slib::shader_library lib;
    CHECK(!lib.can_compile(slib::shader_language::hlsl, sg::shader_format::dxil));

    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
    CHECK(lib.can_compile(slib::shader_language::hlsl, sg::shader_format::dxil));
    CHECK(!lib.can_compile(slib::shader_language::hlsl, sg::shader_format::spirv));

    CHECK(lib.supported_formats(slib::shader_language::hlsl).size() == 1);
}

TEST("slib - a second compiler for the same edge replaces the first")
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));

    CHECK(lib.supported_formats(slib::shader_language::hlsl).size() == 1);
}

TEST("slib - acquiring a format with no compiler fails on the async channel")
{
    slib::shader_library lib;
    test_package pkg;
    lib.add_package(pkg.package(), make_sources());

    // No compiler registered at all: a missing edge is an error to handle, not a crash.
    auto const shader = pkg.invert->acquire(sg::shader_format::dxil);
    REQUIRE(shader != nullptr);
    CHECK(shader->is_ready());
    CHECK(shader->has_error());
}

TEST("slib - acquiring a shader whose source is missing fails on the async channel")
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));

    test_package pkg;
    lib.add_package(pkg.package(), std::make_shared<slib::memory_filesystem>()); // empty filesystem

    auto const shader = pkg.invert->acquire(sg::shader_format::dxil);
    REQUIRE(shader != nullptr);
    CHECK(shader->has_error());
}

TEST("slib - a shader that does not compile fails on the async channel")
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));

    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("invert.hlsl", slib_test::k_broken_source);
    fs->write("blit.hlsl", "blit body");

    test_package pkg;
    lib.add_package(pkg.package(), fs);

    auto const shader = pkg.invert->acquire(sg::shader_format::dxil);
    REQUIRE(shader != nullptr);
    CHECK(shader->has_error());

    // A broken shader must not stop its neighbours from working.
    CHECK(await(pkg.blit_vs->acquire(sg::shader_format::dxil)).entry_point == "main_vs");
}

TEST("slib - includes are inlined and recorded as dependencies")
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));

    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("invert.hlsl", "before #include \"common.hlsli\" after");
    fs->write("common.hlsli", "COMMON");
    fs->write("blit.hlsl", "blit body");

    test_package pkg;
    lib.add_package(pkg.package(), fs);

    CHECK(pkg.invert->dependencies().empty()); // nothing known before the first compile

    auto const shader = pkg.invert->acquire(sg::shader_format::dxil);
    CHECK(fake_compiler::source_of(await(shader)) == "before COMMON after");

    // The include is tracked, which is what lets a later edit to it reload this shader.
    auto const dependencies = pkg.invert->dependencies();
    REQUIRE(dependencies.size() == 2);
    CHECK(dependencies[0] == "test_pkg/invert.hlsl");
    CHECK(dependencies[1] == "test_pkg/common.hlsli");
}

TEST("slib - an include resolves relative to the including file first")
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));

    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("dir/invert.hlsl", "#include \"common.hlsli\"");
    fs->write("dir/common.hlsli", "SIBLING");
    fs->write("common.hlsli", "ROOT");
    fs->write("blit.hlsl", "blit body");

    slib::shader_asset_handle invert;
    slib::shader_definition definitions[] = {
        {.path = "dir/invert.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &invert},
    };
    lib.add_package(slib::shader_package{.name = "test_pkg", .definitions = definitions}, fs);

    // Like a C `"..."` include: the sibling wins over the one at the package root.
    CHECK(fake_compiler::source_of(await(invert->acquire(sg::shader_format::dxil))) == "SIBLING");
    CHECK(invert->dependencies()[1] == "test_pkg/dir/common.hlsli");
}

TEST("slib - an include falls back to the package root")
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));

    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("dir/invert.hlsl", "#include \"common.hlsli\"");
    fs->write("common.hlsli", "ROOT"); // no sibling copy this time

    slib::shader_asset_handle invert;
    slib::shader_definition definitions[] = {
        {.path = "dir/invert.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &invert},
    };
    lib.add_package(slib::shader_package{.name = "test_pkg", .definitions = definitions}, fs);

    CHECK(fake_compiler::source_of(await(invert->acquire(sg::shader_format::dxil))) == "ROOT");
    CHECK(invert->dependencies()[1] == "test_pkg/common.hlsli");
}

TEST("slib - a shared mount serves includes to several packages by a stable path")
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));

    // The shared library lives wherever it likes; the mount point is the contract.
    auto shared = std::make_shared<slib::memory_filesystem>();
    shared->write("brdf.hlsli", "BRDF");
    lib.mount("common", shared);

    auto a_fs = std::make_shared<slib::memory_filesystem>();
    a_fs->write("a.hlsl", "#include \"common/brdf.hlsli\"");
    auto b_fs = std::make_shared<slib::memory_filesystem>();
    b_fs->write("b.hlsl", "#include \"common/brdf.hlsli\"");

    slib::shader_asset_handle a;
    slib::shader_definition a_defs[]
        = {{.path = "a.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &a}};
    slib::shader_asset_handle b;
    slib::shader_definition b_defs[]
        = {{.path = "b.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &b}};

    lib.add_package(slib::shader_package{.name = "pkg_a", .definitions = a_defs}, a_fs);
    lib.add_package(slib::shader_package{.name = "pkg_b", .definitions = b_defs}, b_fs);

    // Both packages reach it by the same virtual path, neither knows where it really is.
    CHECK(fake_compiler::source_of(await(a->acquire(sg::shader_format::dxil))) == "BRDF");
    CHECK(fake_compiler::source_of(await(b->acquire(sg::shader_format::dxil))) == "BRDF");
    CHECK(a->dependencies()[1] == "common/brdf.hlsli");
}

TEST("slib - a missing include fails on the async channel")
{
    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));

    auto fs = std::make_shared<slib::memory_filesystem>();
    fs->write("invert.hlsl", "#include \"nope.hlsli\"");

    slib::shader_asset_handle invert;
    slib::shader_definition definitions[] = {
        {.path = "invert.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &invert},
    };
    lib.add_package(slib::shader_package{.name = "test_pkg", .definitions = definitions}, fs);

    CHECK(invert->acquire(sg::shader_format::dxil)->has_error());
}
