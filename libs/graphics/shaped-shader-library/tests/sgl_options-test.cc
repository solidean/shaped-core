#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
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

using scale_options = slib_test::sgl_shaders::scaled_scale_t::options;
} // namespace

TEST("slib sgl options - the generated struct holds the options an entry point reaches, at the source's defaults")
{
    auto const defaults = scale_options{};
    CHECK(defaults.factor == 2);
    CHECK(defaults.negate == false);
    CHECK(defaults.tile == 64);
    auto const values = scale_options{.factor = 3, .negate = true}.values();
    REQUIRE(values.size() == 3);
    CHECK(values[0].name == "factor");
    CHECK(values[0].value == "3");
    CHECK(values[1].value == "true");
    CHECK(values[2].value == "64");
    CHECK(slib::option_of("format", sg::pixel_format::rgba16_float).value == ".rgba16_float");
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
