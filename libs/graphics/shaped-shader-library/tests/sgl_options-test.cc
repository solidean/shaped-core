#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/impl/kept_builds.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-shader-library/shader_package.hh>
#include <slib_test_sgl_shaders.hh>

#include <memory>

using namespace cc::primitive_defines;

// An SGL entry point's options, set per acquire: each set of values it reaches is a compile of its own.
// The WGSL edge needs no toolchain, so this runs everywhere.

namespace
{
/// The text of a settled WGSL compile, which is its bytecode.
cc::string text_of(sg::async_compiled_shader const& shader)
{
    REQUIRE(shader != nullptr);
    if (shader->has_error())
        FAIL(shader->try_error()->underlying().to_string());
    REQUIRE(shader->has_value());
    auto const& bytes = shader->try_value()->bytecode;
    return cc::string(cc::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()));
}

using scale_options = slib_test::sgl_shaders::scaled_options;
} // namespace

TEST("slib sgl options - the generated struct holds a file's options at the source's defaults, which each wrapper "
     "takes")
{
    auto const defaults = scale_options{};
    CHECK(defaults.factor == 2);
    CHECK(defaults.negate == false);
    CHECK(defaults.tile == 64);
    CHECK(defaults.unreached == 7);
    auto const values = scale_options{.factor = 3, .negate = true}.values();
    REQUIRE(values.size() == 4);
    CHECK(values[0].name == "factor");
    CHECK(values[0].value == "3");
    CHECK(values[1].value == "true");
    CHECK(values[2].value == "64");
    CHECK(values[3].name == "unreached");
    CHECK(slib::option_of("format", sg::pixel_format::rgba16_float).value == ".rgba16_float");

    // the wrapper names the file's struct, and states the options it reaches, which are all its compiles key on
    auto const taken = slib_test::sgl_shaders::scaled_scale_t::options{.tile = 32};
    scale_options const* const same = &taken;
    CHECK(same->tile == 32);
    auto const reached = slib_test::sgl_shaders::scaled_scale_t::reached_options;
    REQUIRE(reached.size() == 3);
    CHECK(reached[0] == "factor");
    CHECK(reached[1] == "negate");
    CHECK(reached[2] == "tile");
}

TEST("slib sgl options - one entry point under two sets of values is two shaders, each compiled once",
     exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    lib.add_package(slib_test::sgl_shaders::package());
    auto const& asset = slib_test::sgl_shaders::scaled.scale.asset;
    REQUIRE(asset != nullptr);
    CHECK(asset->options().size() == 3);

    auto const plain = asset->acquire(sg::shader_format::wgsl, scale_options{}.values());
    auto const wide = asset->acquire(sg::shader_format::wgsl, scale_options{.negate = true, .tile = 32}.values());
    CHECK(plain != wide);
    CHECK(text_of(plain).contains("@workgroup_size(64, 1, 1)"));
    CHECK(text_of(wide).contains("@workgroup_size(32, 1, 1)"));
    CHECK(text_of(plain) != text_of(wide));

    // the same values, in any order, and an option the entry point does not reach, are the same compile
    CHECK(asset->acquire(sg::shader_format::wgsl, scale_options{}.values()) == plain);
    // a default left out is a key of its own, whose text is the same, which a compiler's cache keys on
    CHECK(text_of(asset->acquire(sg::shader_format::wgsl)) == text_of(plain));
    slib::shader_option const reordered[] = {{.name = "tile", .value = "32"},
                                             {.name = "unrelated", .value = "1"},
                                             {.name = "negate", .value = "true"},
                                             {.name = "factor", .value = "2"}};
    CHECK(asset->acquire(sg::shader_format::wgsl, reordered) == wide);
}

TEST("slib sgl options - a value the source refuses is the shader's error", exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    lib.add_package(slib_test::sgl_shaders::package());
    slib::shader_option const wrong[] = {{.name = "tile", .value = "wide"}};
    auto const shader = slib_test::sgl_shaders::scaled.scale.asset->acquire(sg::shader_format::wgsl, wrong);
    REQUIRE(shader != nullptr);
    REQUIRE(shader->has_error());
    CHECK(shader->try_error()->underlying().to_string().contains("invalid-option"));
}

TEST("slib sgl options - a reload recompiles every set of values acquired so far", exclusive("slib-shader-library"))
{
    auto const fs = std::make_shared<slib::memory_filesystem>();
    fs->write("tone.sgl", "@option const bright = false\n"
                          "\n"
                          "binding tone_work:\n"
                          "    values: mut buffer[float]\n"
                          "\n"
                          "@compute(64) fun tone(@thread_id id: int3){tone_work}:\n"
                          "    tone_work.values[id.x] = if bright => 1.0 else 0.25\n");
    auto asset = slib::shader_asset_handle();
    cc::string_view const reached[] = {"bright"};
    slib::shader_definition const definitions[] = {
        {.path = "tone.sgl", .stage = sg::shader_stage::compute, .entry_point = "tone", .options = reached, .asset = &asset},
    };
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    lib.add_package({.name = "tone_pkg", .language = slib::shader_language::sgl, .definitions = definitions}, fs);

    slib::shader_option const bright[] = {{.name = "bright", .value = "true"}};
    CHECK(text_of(asset->acquire(sg::shader_format::wgsl)).contains("0.25"));
    CHECK(text_of(asset->acquire(sg::shader_format::wgsl, bright)).contains("= 1.0"));
    lib.start_hot_reload({.unthreaded = true});

    fs->write("tone.sgl", "@option const bright = false\n"
                          "\n"
                          "binding tone_work:\n"
                          "    values: mut buffer[float]\n"
                          "\n"
                          "@compute(64) fun tone(@thread_id id: int3){tone_work}:\n"
                          "    tone_work.values[id.x] = if bright => 2.0 else 0.5\n");
    lib.poll_hot_reload();

    // each acquire promotes its own set's staged compile
    CHECK(text_of(asset->acquire(sg::shader_format::wgsl)).contains("0.5"));
    CHECK(text_of(asset->acquire(sg::shader_format::wgsl, bright)).contains("= 2.0"));
}

TEST("slib sgl options - a pipeline keeps what it was built with per context and per set of values")
{
    // What a pipeline falls back to once a reload moves its frozen part: two sets described on one context are two
    // builds, and each set gets its own back, in whatever order its values are given.
    // only compared, never followed, so two distinct addresses stand for two contexts
    int const contexts[2] = {};
    auto kept = slib::impl::kept_builds<int>();
    auto const* const ctx = reinterpret_cast<sg::context const*>(&contexts[0]);
    auto const* const other_ctx = reinterpret_cast<sg::context const*>(&contexts[1]);
    slib::shader_option const narrow[] = {{.name = "tile", .value = "8"}, {.name = "sharpen", .value = "false"}};
    slib::shader_option const wide[] = {{.name = "tile", .value = "16"}};
    slib::shader_option const narrow_reordered[]
        = {{.name = "sharpen", .value = "false"}, {.name = "tile", .value = "8"}};

    auto built = 8;
    CHECK(kept.keep_or_restore(ctx, narrow, false, built));
    built = 16;
    CHECK(kept.keep_or_restore(ctx, wide, false, built));

    // the frozen part moved: each set is handed the build its own values had, not the last one described
    built = 99;
    CHECK(kept.keep_or_restore(ctx, narrow_reordered, true, built));
    CHECK(built == 8);
    built = 99;
    CHECK(kept.keep_or_restore(ctx, wide, true, built));
    CHECK(built == 16);

    // a context or a set never described before the move has nothing to fall back to
    built = 99;
    CHECK(!kept.keep_or_restore(other_ctx, narrow, true, built));
    slib::shader_option const unseen[] = {{.name = "tile", .value = "32"}};
    CHECK(!kept.keep_or_restore(ctx, unseen, true, built));
    CHECK(built == 99);
}

TEST("slib sgl options - an entry point listed on its own knows its options, and a value is a compile of its own",
     exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    lib.add_package(slib_test::sgl_shaders::package());
    // `listed.sgl:compute:spread` alone: no `*`, and no generated type for its binding, so a plain handle
    auto const& asset = slib_test::sgl_shaders::listed.spread;
    REQUIRE(asset != nullptr);
    REQUIRE(asset->options().size() == 1);
    CHECK(asset->options()[0] == "tile");

    slib::shader_option const narrow[] = {{.name = "tile", .value = "32"}};
    auto const defaults = asset->acquire(sg::shader_format::wgsl);
    auto const narrowed = asset->acquire(sg::shader_format::wgsl, narrow);
    CHECK(defaults != narrowed);
    CHECK(text_of(defaults).contains("@workgroup_size(64, 1, 1)"));
    CHECK(text_of(narrowed).contains("@workgroup_size(32, 1, 1)"));
}

TEST("slib sgl options - a module's option is a member named after its module, set apart from the program's own",
     exclusive("slib-shader-library"))
{
    using tapped_options = slib_test::sgl_shaders::tapped_options;
    auto const defaults = tapped_options{};
    CHECK(defaults.taps == 2);
    CHECK(defaults.weights.taps == 4);
    auto const values = tapped_options{.weights = {.taps = 8}}.values();
    REQUIRE(values.size() == 2);
    CHECK(values[0].name == "taps");
    CHECK(values[1].name == "weights.taps");
    CHECK(values[1].value == "8");
    auto const reached = slib_test::sgl_shaders::tapped_tap_t::reached_options;
    REQUIRE(reached.size() == 2);
    CHECK(reached[0] == "weights.taps");
    CHECK(reached[1] == "taps");

    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    lib.add_package(slib_test::sgl_shaders::package());
    auto const& asset = slib_test::sgl_shaders::tapped.tap.asset;
    REQUIRE(asset != nullptr);
    auto const plain = text_of(asset->acquire(sg::shader_format::wgsl, tapped_options{}.values()));
    auto const by_module
        = text_of(asset->acquire(sg::shader_format::wgsl, tapped_options{.weights = {.taps = 8}}.values()));
    auto const by_own = text_of(asset->acquire(sg::shader_format::wgsl, tapped_options{.taps = 8}.values()));
    CHECK(plain != by_module);
    CHECK(plain != by_own);
    CHECK(by_module != by_own);
}

TEST("slib sgl options - a name given twice is logged as an error, and the first value is taken",
     exclusive("slib-shader-library"))
{
    slib::shader_library lib;
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    lib.add_package(slib_test::sgl_shaders::package());
    nx::expect_error("given the option tile twice", nx::exactly(1));
    slib::shader_option const twice[] = {{.name = "tile", .value = "32"}, {.name = "tile", .value = "16"}};
    auto const shader = slib_test::sgl_shaders::scaled.scale.asset->acquire(sg::shader_format::wgsl, twice);
    CHECK(text_of(shader).contains("@workgroup_size(32, 1, 1)"));
}
