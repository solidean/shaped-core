#include "fake_compiler.hh"

#include <nexus/test.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/filesystem/real_filesystem.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-shader-library/shader_package.hh>

#include <memory>

// The last test in this file is the one exception to "no disk, no sleeps" below, and it earns it: nothing
// else here runs a real directory, a real OS watch and a real thread together, which is precisely the path
// a dev build takes. It needs <filesystem> to make an edit the OS can see.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

// Hot reload, with no disk and no sleeps: a memory_filesystem write is the edit, and an unthreaded
// watcher makes the scan happen exactly when the test says so. That determinism is what the virtual
// filesystem buys — the same paths through the watcher run threaded in a real app.
//
// memory_filesystem fires its watch straight from write(), so these run the notify path end to end without
// an OS or a timer anywhere. counting_filesystem below is how the last few tell "the watcher rescanned"
// apart from "the watcher never woke", which is the difference the whole design exists for.

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

/// A memory_filesystem that counts what the watcher asks of it, and can be told to report that it cannot
/// notify. A scan *is* a run of revision() calls, so counting them is how a test observes whether the
/// watcher woke at all — and `can_watch` is what a platform with no watch backend looks like from here.
struct counting_filesystem final : slib::filesystem
{
    explicit counting_filesystem(bool can_watch) : _can_watch(can_watch) {}

    [[nodiscard]] cc::optional<cc::string> read_text(cc::string_view path) const override
    {
        return _inner.read_text(path);
    }

    [[nodiscard]] slib::file_revision revision(cc::string_view path) const override
    {
        ++revision_calls;
        return _inner.revision(path);
    }

    [[nodiscard]] cc::optional<slib::watch_subscription> watch(cc::string_view prefix, slib::watch_sink sink) const override
    {
        if (!_can_watch)
            return cc::nullopt;
        return _inner.watch(prefix, cc::move(sink));
    }

    void write(cc::string_view path, cc::string_view text) { _inner.write(path, text); }

    mutable int revision_calls = 0;

private:
    slib::memory_filesystem _inner;
    bool _can_watch;
};

/// reload_fixture over a counting_filesystem, with one shader already acquired so the watcher has
/// something to watch.
struct counting_fixture
{
    std::shared_ptr<counting_filesystem> fs;
    slib::shader_library lib;
    test_package pkg;

    explicit counting_fixture(bool can_watch) : fs(std::make_shared<counting_filesystem>(can_watch))
    {
        fs->write("invert.hlsl", "v1");
        lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
        lib.add_package(pkg.package(), fs);
    }

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

TEST("slib - a shader first acquired after hot reload started is still watched")
{
    reload_fixture f;
    f.start();
    f.lib.poll_hot_reload(); // nothing is acquired yet, so this finds no dependencies and watches nothing

    // The trap: a shader only reports what it is built from once a compile has resolved its includes, and
    // the first acquire does that here — on a consumer's thread, after the watcher already went quiet. The
    // watcher reads dependencies() rather than being handed them, so unless acquire tells it to look again
    // it stays parked over an empty watch list and every edit below is lost.
    CHECK(f.source() == "v1");
    f.lib.poll_hot_reload(); // only has anything to do because acquire asked for it

    f.fs->write("invert.hlsl", "v2");
    f.lib.poll_hot_reload();
    CHECK(f.source() == "v2");
}

TEST("slib - a watched poll does no work while nothing changes")
{
    counting_fixture f{true};
    CHECK(f.source() == "v1");

    f.lib.start_hot_reload({.unthreaded = true});
    f.lib.poll_hot_reload(); // settles: the scan on start, and the one its first subscription asks for
    f.lib.poll_hot_reload();

    // The whole point of the exercise. Polling, each of these re-stats every dependency of every asset —
    // 20-50 ms at a couple of hundred shaders, every interval, forever, whether or not anyone is editing.
    auto const calls_before = f.fs->revision_calls;
    f.lib.poll_hot_reload();
    f.lib.poll_hot_reload();
    CHECK(f.fs->revision_calls == calls_before);

    // ...and an edit still lands, because the filesystem says so rather than a timer noticing eventually.
    f.fs->write("invert.hlsl", "v2");
    f.lib.poll_hot_reload();
    CHECK(f.fs->revision_calls > calls_before);
    CHECK(f.source() == "v2");
}

TEST("slib - hot reload falls back to polling when the filesystem cannot notify")
{
    counting_fixture f{false};
    CHECK(f.source() == "v1");

    f.lib.start_hot_reload({.unthreaded = true});
    f.lib.poll_hot_reload();

    // Nothing will ever wake this watcher, so every poll has to rescan — the behaviour every platform
    // without a watch backend keeps, and the reason watch() may answer "poll me" rather than lie.
    auto const calls_before = f.fs->revision_calls;
    f.lib.poll_hot_reload();
    CHECK(f.fs->revision_calls > calls_before);

    f.fs->write("invert.hlsl", "v2");
    f.lib.poll_hot_reload();
    CHECK(f.source() == "v2");
}

TEST("slib - force_polling ignores a filesystem that could notify")
{
    counting_fixture f{true}; // it *can* notify; the config says not to care
    CHECK(f.source() == "v1");

    f.lib.start_hot_reload({.unthreaded = true, .force_polling = true});

    auto const calls_before = f.fs->revision_calls;
    f.lib.poll_hot_reload();
    CHECK(f.fs->revision_calls > calls_before);

    f.fs->write("invert.hlsl", "v2");
    f.lib.poll_hot_reload();
    CHECK(f.source() == "v2");
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

TEST("slib - a threaded watcher reloads through a real directory")
{
    // The whole thing, on the path a dev build actually takes: a real directory, the OS watch backend, a
    // thread, and no polling anywhere. Everything above runs on memory_filesystem so it can be exact — but
    // none of it would notice a backend that never fires, or a notification that never reaches the mailbox.
    auto const dir = std::filesystem::temp_directory_path() / "slib-threaded-real-reload";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    auto const write = [&](char const* text)
    {
        std::ofstream out(dir / "invert.hlsl", std::ios::binary | std::ios::trunc);
        out << text;
    };
    write("v1");

    slib::shader_library lib;
    test_package pkg;
    lib.add_compiler(std::make_unique<fake_compiler>(slib::shader_language::hlsl, sg::shader_format::dxil));
    lib.add_package(pkg.package(), std::make_shared<slib::real_filesystem>(cc::string(dir.string().c_str())));

    auto const source = [&]
    {
        auto const shader = pkg.invert->acquire(sg::shader_format::dxil);
        REQUIRE(shader->has_value());
        return cc::string(fake_compiler::source_of(*shader->try_value()));
    };
    CHECK(source() == "v1"); // the first acquire compiles, and tells the watcher what to watch

    // No interval_ms and no unthreaded: on Windows this reloads because the OS said so. A platform with no
    // backend falls back to the default 200 ms poll instead, and one with no threads at all runs unthreaded
    // off poll_hot_reload() below — every one of them has to land well inside the wait.
    lib.start_hot_reload();

    write("a much longer v2");

    bool reloaded = false;
    for (int waited = 0; waited < 5000 && !reloaded; waited += 5)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        lib.poll_hot_reload(); // a no-op while the watcher has its own thread; the whole engine without one
        reloaded = source() == "a much longer v2";
    }
    CHECK(reloaded);

    std::filesystem::remove_all(dir, ec);
}
