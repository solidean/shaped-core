#include "fake_compiler.hh"

#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-shader-library/shader_package.hh>

#include <memory>

// Hot reload, with no disk and no sleeps: a memory_filesystem write is the edit, and an unthreaded
// watcher makes the scan happen exactly when the test says so. That determinism is what the virtual
// filesystem buys — the same paths through the watcher run threaded in a real app.

using slib_test::fake_compiler;

namespace
{
struct test_package
{
    slib::shader_asset_handle invert;

    slib::shader_definition definitions[1] = {
        {.path = "invert.hlsl", .stage = sg::shader_stage::compute, .entry_point = "main", .asset = &invert},
    };

    [[nodiscard]] slib::shader_package package() const
    {
        return slib::shader_package{.name = "test_pkg", .definitions = definitions};
    }
};

/// A library with one package on a memory filesystem and an unthreaded watcher — the whole fixture.
struct reload_fixture
{
    std::shared_ptr<slib::memory_filesystem> fs = std::make_shared<slib::memory_filesystem>();
    slib::shader_library lib;
    test_package pkg;

    explicit reload_fixture(cc::string_view initial_source = "v1")
    {
        fs->write("invert.hlsl", initial_source);
        lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
        lib.add_package(pkg.package(), fs);
    }

    void start() { lib.start_hot_reload({.unthreaded = true}); }

    /// The shader text currently acquired for dxil.
    [[nodiscard]] cc::string source()
    {
        auto const shader = pkg.invert->acquire(sg::shader_format::dxil);
        REQUIRE(shader->has_value());
        return cc::string(fake_compiler::source_of(*shader->try_value()));
    }
};
} // namespace

TEST("slib - editing a shader reloads it")
{
    reload_fixture f;
    CHECK(f.source() == "v1"); // the first acquire compiles it
    f.start();

    auto const generation_before = f.pkg.invert->generation();

    f.fs->write("invert.hlsl", "v2");
    f.lib.poll_hot_reload(); // the watcher notices and stages a recompile

    CHECK(f.source() == "v2"); // acquire promotes it
    CHECK(f.pkg.invert->generation() > generation_before);
    CHECK(f.lib.generation() > 0); // the coarse "something changed" check moved too
}

TEST("slib - a scan with nothing changed reloads nothing")
{
    reload_fixture f;
    CHECK(f.source() == "v1");
    f.start();

    auto const generation_before = f.pkg.invert->generation();
    f.lib.poll_hot_reload();
    f.lib.poll_hot_reload();

    CHECK(f.source() == "v1");
    CHECK(f.pkg.invert->generation() == generation_before);
    CHECK(f.lib.generation() == 0);
}

TEST("slib - a shader never acquired is not compiled by a reload")
{
    reload_fixture f;
    f.start();

    // Nothing acquired yet, so the watcher has no dependencies to watch and nothing to rebuild.
    f.fs->write("invert.hlsl", "v2");
    f.lib.poll_hot_reload();
    CHECK(f.pkg.invert->generation() == 0);

    // The first acquire simply compiles the current text.
    CHECK(f.source() == "v2");
}

TEST("slib - a broken edit keeps the last good shader and reports why")
{
    reload_fixture f;
    CHECK(f.source() == "v1");
    f.start();

    f.fs->write("invert.hlsl", slib_test::k_broken_source);
    f.lib.poll_hot_reload();

    // The point of staging rather than replacing: a shader that no longer compiles must not take the
    // running one down with it.
    CHECK(f.source() == "v1");
    REQUIRE(f.pkg.invert->last_error().has_value());
    CHECK(f.pkg.invert->last_error().value().contains("fake compile error"));

    SECTION("and recovers once the edit is fixed")
    {
        f.fs->write("invert.hlsl", "v3");
        f.lib.poll_hot_reload();

        CHECK(f.source() == "v3");
        CHECK(!f.pkg.invert->last_error().has_value()); // cleared by the successful promotion
    }
}

TEST("slib - deleting a shader's source keeps the last good shader")
{
    reload_fixture f;
    CHECK(f.source() == "v1");
    f.start();

    f.fs->remove("invert.hlsl");
    f.lib.poll_hot_reload();

    CHECK(f.source() == "v1");
    CHECK(f.pkg.invert->last_error().has_value());
}

TEST("slib - editing an included file reloads the shaders that include it")
{
    reload_fixture f("#include \"common.hlsli\"");
    f.fs->write("common.hlsli", "COMMON_V1");

    CHECK(f.source() == "COMMON_V1");
    CHECK(f.pkg.invert->dependencies().size() == 2);
    f.start();

    // The shader's own file is untouched — only something it pulled in changed. This is what recording
    // every resolved include during preprocess is for.
    f.fs->write("common.hlsli", "COMMON_V2");
    f.lib.poll_hot_reload();

    CHECK(f.source() == "COMMON_V2");
    CHECK(f.pkg.invert->generation() == 1);
}

TEST("slib - an include discovered by a reload is seeded, not treated as a change")
{
    reload_fixture f("no includes yet");
    CHECK(f.source() == "no includes yet");
    f.fs->write("common.hlsli", "COMMON");
    f.start();

    // The edit both changes the shader and makes it depend on a file the watcher has never seen.
    f.fs->write("invert.hlsl", "#include \"common.hlsli\"");
    f.lib.poll_hot_reload();
    CHECK(f.source() == "COMMON");
    CHECK(f.pkg.invert->generation() == 1);

    // The newly discovered include must not read as changed on the next scan — that would reload the
    // shader forever, once per poll.
    f.lib.poll_hot_reload();
    CHECK(f.pkg.invert->generation() == 1);
}

TEST("slib - each acquired format is reloaded")
{
    reload_fixture f;
    f.lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::spirv));

    CHECK(f.source() == "v1");
    auto const spirv_before = f.pkg.invert->acquire(sg::shader_format::spirv);
    CHECK(fake_compiler::source_of(*spirv_before->try_value()) == "v1");
    f.start();

    f.fs->write("invert.hlsl", "v2");
    f.lib.poll_hot_reload();

    CHECK(f.source() == "v2");
    auto const spirv_after = f.pkg.invert->acquire(sg::shader_format::spirv);
    CHECK(fake_compiler::source_of(*spirv_after->try_value()) == "v2");
    CHECK(spirv_after != spirv_before);
}

TEST("slib - poll_hot_reload is a no-op before hot reload is started")
{
    reload_fixture f;
    CHECK(f.source() == "v1");
    CHECK(!f.lib.is_hot_reloading());

    f.fs->write("invert.hlsl", "v2");
    f.lib.poll_hot_reload(); // safe to call unconditionally

    CHECK(f.source() == "v1"); // nothing watches yet
}

TEST("slib - packages must be registered before hot reload starts")
{
    reload_fixture f;
    f.start();
    CHECK(f.lib.is_hot_reloading());

    // The watcher walks the asset list on its own thread; growing it underneath is a data race.
    test_package late;
    CHECK_ASSERTS(f.lib.add_package(late.package(), f.fs));
    CHECK_ASSERTS(f.lib.start_hot_reload());
}

TEST("slib - a threaded watcher starts and stops cleanly")
{
    // The tests above pump by hand for determinism; this one takes the path a real app does. It only
    // asserts the lifecycle — a real reload race would be flaky, and the scan logic is covered above.
    reload_fixture f;
    CHECK(f.source() == "v1");

    f.lib.start_hot_reload({.interval_ms = 1});
    CHECK(f.lib.is_hot_reloading());
    f.lib.poll_hot_reload(); // no-op while the watcher has its own thread

    CHECK(f.source() == "v1");
    // The fixture's destructor stops the watcher; a stop must not wait out the poll interval.
}
