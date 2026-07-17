#include "fake_compiler.hh"

#include <nexus/test.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>

#include <memory>

// The generated package (see sc_add_shader_package in this library's CMakeLists). These pin the codegen
// contract itself: the symbols exist and are typed, the table matches what was declared, and the include
// closure got embedded so a binary with no source tree still has its shaders.
#include <slib_test_shaders.hh>

using slib_test::fake_compiler;

TEST("slib - the generated package exposes a symbol per stage and entry point")
{
    // The payoff of generating C++: this is checked by the compiler, so a typo is a build error rather
    // than a runtime lookup that returns nothing.
    slib::shader_library lib;
    lib.add_package(slib_test::shaders::package());

    REQUIRE(slib_test::shaders::invert.compute.main != nullptr);
    REQUIRE(slib_test::shaders::blit.vertex.main_vs != nullptr);
    REQUIRE(slib_test::shaders::blit.fragment.main_ps != nullptr);

    CHECK(slib_test::shaders::invert.compute.main->stage() == sg::shader_stage::compute);
    CHECK(slib_test::shaders::blit.vertex.main_vs->stage() == sg::shader_stage::vertex);
    CHECK(slib_test::shaders::blit.fragment.main_ps->stage() == sg::shader_stage::fragment);

    CHECK(slib_test::shaders::invert.compute.main->entry_point() == "main");
    CHECK(slib_test::shaders::blit.vertex.main_vs->entry_point() == "main_vs");
    CHECK(slib_test::shaders::blit.fragment.main_ps->entry_point() == "main_ps");

    // The declared path is carried through, not rebuilt from folder + stem.
    CHECK(slib_test::shaders::invert.compute.main->virtual_path() == "slib_test_shaders/invert.hlsl");
    CHECK(slib_test::shaders::blit.vertex.main_vs->virtual_path() == "slib_test_shaders/blit.hlsl");
}

TEST("slib - the generated package describes itself")
{
    auto const& pkg = slib_test::shaders::package();

    CHECK(pkg.name == "slib_test_shaders");
    CHECK(pkg.language == slib::shader_language::hlsl);
    CHECK(pkg.definitions.size() == 3);
    CHECK(!pkg.source_dir.empty()); // baked absolute at configure; present in a dev build

    for (auto const& definition : pkg.definitions)
    {
        CHECK(!definition.path.empty());
        CHECK(!definition.entry_point.empty());
        CHECK(definition.asset != nullptr);
    }
}

TEST("slib - the generated package embeds its include closure")
{
    auto const& pkg = slib_test::shaders::package();

    // Three declared entry points over two files, plus the .hlsli one of them includes. The include is
    // the point: ssc::dxc has no filesystem fallback, so a shipped build that embedded only the entry
    // points could not resolve it.
    REQUIRE(pkg.embedded_files.size() == 3);

    bool has_invert = false;
    bool has_blit = false;
    bool has_include = false;
    for (auto const& file : pkg.embedded_files)
    {
        CHECK(!file.text.empty());
        if (file.path == "invert.hlsl")
            has_invert = true;
        if (file.path == "blit.hlsl")
            has_blit = true;
        if (file.path == "util/common.hlsli")
            has_include = true;
    }
    CHECK(has_invert);
    CHECK(has_blit);
    CHECK(has_include);
}

TEST("slib - the generated package compiles from its embedded sources alone")
{
    // What a shipped binary does: no source tree, only what the generator baked in. Mounting the
    // package with an explicit empty filesystem would be the other extreme; here we take the real
    // package and simply prove the embedded copy is complete enough to build from.
    auto const& pkg = slib_test::shaders::package();

    slib::shader_library lib;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
    lib.add_package(pkg, std::make_shared<slib::embedded_filesystem>(pkg.embedded_files));

    auto const shader = slib_test::shaders::invert.compute.main->acquire(sg::shader_format::dxil);
    REQUIRE(shader->has_value());

    // The fake compiler inlines includes, so a flattened body proves the .hlsli resolved from the
    // embedded copy rather than the disk.
    auto const source = fake_compiler::source_of(*shader->try_value());
    CHECK(source.contains("slib_test_invert"));
    CHECK(!source.contains("#include"));
}
