#include "fake_compiler.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <nexus/tests/logs.hh>
#include <shaped-graphics-language/driver/describe.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-shader-library/shader_package.hh>

#include <memory>

using namespace cc::primitive_defines;

namespace
{
/// One ray-tracing pipeline over a raygen and a miss; `extra` is a float member more of the payload, and the miss writes
/// `color`.
cc::string rt_source(cc::string_view extra, cc::string_view color = "1.0")
{
    return cc::format("require raytracing_pipeline\n"
                      "binding frame:\n    world: acceleration_structure[.triangles]\n    output: mut buffer[float]\n"
                      "struct radiance:\n    color: float\n{}"
                      "rays rs:\n    primary: radiance\n"
                      "@raygen fun start(@launch_id id: int3){{frame}}:\n"
                      "    let mut p = radiance(0.0{})\n"
                      "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), "
                      "rs.primary, mut p)\n"
                      "    frame.output[id.x] = p.color\n"
                      "@miss fun sky(p: mut radiance):\n    p.color = {}\n"
                      "@raytracing pipeline path:\n    rays = rs\n    raygen = start\n    miss.primary = sky\n",
                      extra, extra.empty() ? "" : ", 0.0", color);
}

/// The frozen part `source`'s one ray-tracing pipeline has, as the generator bakes it; kept for the life of the process.
cc::span<cc::string_view const> rt_frozen_of(cc::string_view source)
{
    static auto storage = cc::vector<cc::unique_ptr<cc::vector<cc::string>>>();
    static auto views = cc::vector<cc::unique_ptr<cc::vector<cc::string_view>>>();
    auto const described = sgl::describe({.source = source, .source_name = "rt.sgl"});
    REQUIRE(described.has_value());
    REQUIRE(described.value().raytracing_pipelines.size() == 1);
    storage.push_back(cc::make_unique<cc::vector<cc::string>>(described.value().raytracing_pipelines[0].frozen));
    views.push_back(cc::make_unique<cc::vector<cc::string_view>>());
    for (auto const& line : *storage.back())
        views.back()->push_back(line);
    return *views.back();
}
} // namespace

TEST("slib raytracing pipeline - a reload that moves a payload or a record keeps what the build had",
     exclusive("slib-shader-library"))
{
    auto const fs = std::make_shared<slib::memory_filesystem>();
    fs->write("rt.sgl", rt_source(""));

    // A fake compiler behind SGL's own pass: the text is HLSL for vulkan, and compiling it is instant.
    auto lib = slib::shader_library();
    lib.add_compiler(slib::create_sgl_compiler(
        std::make_unique<slib_test::fake_compiler>(slib::shader_language::hlsl, sg::shader_format::spirv)));
    static auto raygen = slib::shader_asset_handle();
    static auto miss = slib::shader_asset_handle();
    slib::shader_definition const definitions[] = {
        {.path = "rt.sgl", .stage = sg::shader_stage::raygen, .entry_point = "start", .asset = &raygen},
        {.path = "rt.sgl", .stage = sg::shader_stage::miss, .entry_point = "sky", .asset = &miss},
    };
    lib.add_package({.name = "rt_pkg", .language = slib::shader_language::sgl, .definitions = definitions}, fs);
    lib.start_hot_reload({.unthreaded = true});

    // What the generator writes for this source; static like a generated one, since slib keys reload state by it.
    static slib::shader_asset_handle const* const misses[] = {&miss};
    static auto const definition = slib::raytracing_pipeline_definition{.file = "rt.sgl",
                                                                        .name = "path",
                                                                        .raygen = &raygen,
                                                                        .misses = misses,
                                                                        .frozen = rt_frozen_of(rt_source(""))};

    auto const compile = [&]
    {
        REQUIRE(raygen->acquire(sg::shader_format::spirv)->has_value());
        REQUIRE(miss->acquire(sg::shader_format::spirv)->has_value());
    };
    auto const edit = [&](cc::string source)
    {
        fs->write("rt.sgl", cc::move(source));
        lib.poll_hot_reload();
        compile();
    };

    compile();
    // The watcher's first scan is its baseline, so an edit before it would read as the file it already knew.
    lib.poll_hot_reload();
    CHECK(slib::frozen_moved_of(definition) == "");

    // A body the definition does not fix is followed.
    edit(rt_source("", "2.0"));
    REQUIRE(miss->generation() > 0);
    CHECK(slib::frozen_moved_of(definition) == "");

    // A payload one field wider moves the ray set and the payload size the pipeline was described with.
    nx::expect_warning("keeps what it was last built with");
    edit(rt_source("    alpha: float\n", "2.0"));
    auto const widened = slib::frozen_moved_of(definition);
    CHECK(widened.starts_with("rays: rs: primary radiance@"));
    CHECK(widened.contains("max payload size: 4 -> 8\n"));

    // Taking the payload back lets the pipeline follow again.
    edit(rt_source("", "3.0"));
    CHECK(slib::frozen_moved_of(definition) == "");
}
