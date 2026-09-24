#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <nexus/test.hh>
#include <nexus/tests/logs.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/syntax/parsed_file.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-shader-library/compiler/sgl_compiler.hh>
#include <shaped-shader-library/compiler/wgsl_compiler.hh>
#include <shaped-shader-library/filesystem/memory_filesystem.hh>
#include <shaped-shader-library/pipeline.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-shader-library/shader_package.hh>

#include <memory>

using namespace cc::primitive_defines;

// Three things spell sg's raster pipeline description: sg's structs, the SGL prelude's mirror of them, and the table
// slib writes one into the other with.
// Nothing makes them agree but the tests here.

namespace
{
/// The library's prelude, checked alone, so its mirror of the description can be walked.
struct checked_prelude
{
    cc::vector<sgl::parsed_file> files;
    cc::vector<sgl::ast::file_ast> asts;
    sgl::check::checked_module module;
};

checked_prelude check_prelude()
{
    auto result = checked_prelude();
    for (auto const& p : sgl::prelude_files())
        result.files.push_back(sgl::parse(p.source));
    for (auto const& f : result.files)
        result.asts.push_back(sgl::ast::build(f));
    auto prelude = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < result.files.size(); ++i)
        prelude.push_back({.file = result.files[i], .ast = result.asts[i]});
    // An empty program behind the prelude: the prelude's own symbols are what is read.
    static auto const empty = sgl::parse("");
    static auto const empty_ast = sgl::ast::build(empty);
    result.module = sgl::check::check(prelude, {.file = empty, .ast = empty_ast});
    return result;
}

sgl::check::symbol const* symbol_named(sgl::check::checked_module const& m, cc::string_view name)
{
    for (auto const& s : m.symbols)
        if (s.name == name)
            return &s;
    return nullptr;
}

/// Every leaf path below `type`, with `*` for a target, as the SGL compiler writes a setting's path.
void leaves_of(sgl::check::checked_module const& m, sgl::check::type_id type, cc::string prefix, cc::vector<cc::string>& out)
{
    auto const& t = m.at(type);
    auto const is_struct = t.kind == sgl::check::type_kind::structure && m.builtin_type_of(type) == nullptr;
    if (!is_struct)
    {
        out.push_back(cc::move(prefix));
        return;
    }
    for (auto const& member : m.at(t.members))
    {
        auto path = prefix.empty() ? cc::string(member.name) : cc::format("{}.{}", prefix, member.name);
        if (member.name == "color_targets")
            path += ".*";
        leaves_of(m, member.type, cc::move(path), out);
    }
}
} // namespace

TEST("slib pipeline - the settings table takes exactly the leaves of the prelude's description")
{
    auto const p = check_prelude();
    REQUIRE(p.module.diagnostics.empty());
    auto const* const description = symbol_named(p.module, "raster_pipeline_description");
    REQUIRE(description != nullptr);

    auto leaves = cc::vector<cc::string>();
    leaves_of(p.module, description->type, {}, leaves);
    // `blend = .none` is the one path that is no leaf: the optional part itself.
    leaves.push_back("color_targets.*.blend");

    auto const table = slib::settable_paths();
    auto drift = cc::string();
    for (auto const& leaf : leaves)
    {
        auto is_in_table = false;
        for (auto const path : table)
            is_in_table = is_in_table || path == leaf;
        if (!is_in_table)
            drift.appendf("the prelude has {}, and slib's table does not\n", leaf);
    }
    for (auto const path : table)
    {
        auto is_in_prelude = false;
        for (auto const& leaf : leaves)
            is_in_prelude = is_in_prelude || leaf == path;
        if (!is_in_prelude)
            drift.appendf("slib's table has {}, and the prelude does not\n", path);
    }
    CHECK(drift == "");
}

TEST("slib pipeline - every enum of the prelude's description names sg's enumerators, in their order")
{
    auto const p = check_prelude();
    auto drift = cc::string();
    for (auto const name : {"primitive_topology", "fill_mode", "cull_mode", "front_face", "compare_op", "stencil_op",
                            "blend_factor", "blend_op", "pixel_format"})
    {
        auto const* const s = symbol_named(p.module, name);
        REQUIRE(s != nullptr);
        auto const cases = p.module.at(p.module.at(s->type).cases);
        auto const names = slib::enum_case_names(name);
        if (cases.size() != names.size())
            drift.appendf("{}: the prelude has {} cases and sg {}\n", name, cases.size(), names.size());
        for (auto i = isize(0); i < cases.size() && i < names.size(); ++i)
            if (cases[i].name != names[i])
                drift.appendf("{}: case {} is {} in the prelude and {} in sg\n", name, i, cases[i].name, names[i]);
    }
    CHECK(drift == "");
}

TEST("slib pipeline - sg's mirrored structs keep the fields the prelude mirrors")
{
    // A field added to one of these is a setting the prelude does not have yet: add it there and to the table.
    auto [r1, r2, r3, r4, r5, r6, r7] = sg::rasterization_state{};
    auto [s1, s2, s3, s4] = sg::stencil_face{};
    auto [d1, d2, d3, d4, d5, d6, d7, d8] = sg::depth_stencil_state{};
    auto [b1, b2, b3] = sg::blend_component{};
    auto [c1, c2] = sg::blend_state{};
    auto [t1, t2, t3] = sg::color_target_state{};
    auto [p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16] = sg::raster_pipeline_description{};
    SUCCEED();
}

TEST("slib pipeline - a setting of each kind writes its field, and a target is found by its name")
{
    auto desc = sg::raster_pipeline_description();
    desc.color_targets.push_back({});
    desc.color_targets.push_back({});
    cc::string_view const targets[] = {"albedo", "normal"};
    slib::pipeline_setting const settings[] = {
        {.path = "rasterization.cull", .kind = slib::setting_kind::enum_case, .enum_case = "front"},
        {.path = "depth_stencil.depth_test", .kind = slib::setting_kind::boolean, .integer = 1},
        {.path = "rasterization.depth_bias", .kind = slib::setting_kind::real, .real = -2.5},
        {.path = "depth_stencil.stencil_read_mask", .kind = slib::setting_kind::integer, .integer = 15},
        {.path = "color_targets.normal.format", .kind = slib::setting_kind::enum_case, .enum_case = "rgba16_float"},
        {.path = "color_targets.normal.blend.color.source", .kind = slib::setting_kind::enum_case, .enum_case = "src_alpha"},
        {.path = "color_targets.albedo.write_mask.a", .kind = slib::setting_kind::boolean, .integer = 0},
        {.path = "sample_count", .kind = slib::setting_kind::host},
    };
    REQUIRE(slib::apply_settings(desc, settings, targets).has_value());

    CHECK(desc.rasterization.cull == sg::cull_mode::front);
    CHECK(desc.depth_stencil.depth_test);
    CHECK(desc.rasterization.depth_bias == -2.5f);
    CHECK(desc.depth_stencil.stencil_read_mask == 15);
    CHECK(desc.color_targets[0].format == sg::pixel_format::undefined);
    CHECK(desc.color_targets[1].format == sg::pixel_format::rgba16_float);
    // Writing one field of a blend switches it on, over sg's defaults for the rest.
    CHECK(!desc.color_targets[0].blend.has_value());
    REQUIRE(desc.color_targets[1].blend.has_value());
    CHECK(desc.color_targets[1].blend.value().color.source == sg::blend_factor::src_alpha);
    CHECK(desc.color_targets[1].blend.value().alpha == sg::blend_component{});
    CHECK(!desc.color_targets[0].write_mask.has(sg::color_channel::a));
    CHECK(desc.color_targets[0].write_mask.has(sg::color_channel::r));
    // What the host states arrives later, so a `.host` setting writes nothing.
    CHECK(desc.sample_count == 1);

    slib::pipeline_setting const off[] = {{.path = "color_targets.normal.blend", .kind = slib::setting_kind::none}};
    REQUIRE(slib::apply_settings(desc, off, targets).has_value());
    CHECK(!desc.color_targets[1].blend.has_value());
}

TEST("slib pipeline - a setting a description cannot take is an error that names it")
{
    auto desc = sg::raster_pipeline_description();
    desc.color_targets.push_back({});
    cc::string_view const targets[] = {"color"};
    auto const error_of = [&](slib::pipeline_setting const& s)
    {
        slib::pipeline_setting const one[] = {s};
        auto const r = slib::apply_settings(desc, one, targets);
        return r.has_value() ? cc::string() : r.error();
    };
    CHECK(error_of({.path = "rasterization.culling", .kind = slib::setting_kind::boolean})
          == "rasterization.culling: sg's raster_pipeline_description has no such field");
    CHECK(error_of({.path = "rasterization.cull", .kind = slib::setting_kind::enum_case, .enum_case = "sideways"})
          == "rasterization.cull: the field cannot take this value");
    CHECK(error_of({.path = "color_targets.normal.format", .kind = slib::setting_kind::enum_case, .enum_case = "r8_unorm"})
          == "color_targets.normal.format: the pipeline has no target normal");
}

namespace
{
/// One pipeline over two stages; `extra` is appended to its block, which is what each reload edits.
cc::string reload_source(cc::string_view extra)
{
    return cc::string("@vertex struct vin:\n    p: pos3\n"
                      "struct link:\n    @position p: hpos4\n"
                      "@pixel struct target:\n    color: float4\n"
                      "@vertex fun vs(v: vin) -> link:\n    return { p = hpos4(..v.p, 1.0) }\n"
                      "@pixel fun ps(l: link) -> target:\n    return { color = float4(1.0, 1.0, 1.0, 1.0) }\n"
                      "pipeline:\n    vertex = vs\n    pixel = ps\n    format = .host\n")
         + extra;
}

/// What a pipeline's settings say, one `path = value` per line.
cc::string text_of(cc::span<slib::pipeline_setting const> settings)
{
    auto out = cc::string();
    for (auto const& s : settings)
    {
        out.appendf("{} = ", s.path);
        switch (s.kind)
        {
        case slib::setting_kind::enum_case:
            out.appendf(".{}", s.enum_case);
            break;
        case slib::setting_kind::integer:
            out.appendf("{}", s.integer);
            break;
        case slib::setting_kind::host:
            out += ".host";
            break;
        default:
            out += "?";
            break;
        }
        out += "\n";
    }
    return out;
}
} // namespace

TEST("slib pipeline - a reload moves a pipeline's configuration, and never its frozen part",
     exclusive("slib-shader-library"))
{
    auto const fs = std::make_shared<slib::memory_filesystem>();
    fs->write("pipeline.sgl", reload_source(""));

    auto lib = slib::shader_library();
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    auto vs = slib::shader_asset_handle();
    auto ps = slib::shader_asset_handle();
    slib::shader_definition const definitions[] = {
        {.path = "pipeline.sgl", .stage = sg::shader_stage::vertex, .entry_point = "vs", .asset = &vs},
        {.path = "pipeline.sgl", .stage = sg::shader_stage::fragment, .entry_point = "ps", .asset = &ps},
    };
    lib.add_package({.name = "reload_pkg", .language = slib::shader_language::sgl, .definitions = definitions}, fs);
    lib.start_hot_reload({.unthreaded = true});

    // What the generator writes for this source: the definition the host's code was built against.
    cc::string_view const targets[] = {"color"};
    slib::pipeline_setting const baked[] = {
        {.path = "color_targets.color.format", .kind = slib::setting_kind::host},
    };
    auto const definition = slib::pipeline_definition{.file = "pipeline.sgl",
                                                      .name = "pipeline",
                                                      .vertex = &vs,
                                                      .pixel = &ps,
                                                      .targets = targets,
                                                      .settings = baked,
                                                      .vertex_input_name = "vin",
                                                      .target_struct = "target"};

    // A WGSL compile settles at once, so acquiring is what compiles, and after a reload what promotes.
    auto const compile = [&]
    {
        REQUIRE(vs->acquire(sg::shader_format::wgsl)->has_value());
        REQUIRE(ps->acquire(sg::shader_format::wgsl)->has_value());
    };
    auto const edit = [&](cc::string_view extra)
    {
        fs->write("pipeline.sgl", reload_source(extra));
        lib.poll_hot_reload();
        compile();
    };

    compile();
    // The watcher's first scan is its baseline, so an edit before it would read as the file it already knew.
    lib.poll_hot_reload();
    // Nothing reloaded: the build's own settings, and nothing read.
    CHECK(text_of(slib::configuration_of(definition).settings) == "color_targets.color.format = .host\n");

    // Configuration follows the source.
    edit("    cull = .front\n");
    auto const followed = slib::configuration_of(definition);
    CHECK(followed.frozen_moved == "");
    CHECK(text_of(followed.settings) == "color_targets.color.format = .host\nrasterization.cull = .front\n");

    // A format is frozen: the host created its targets in it, so the configuration stays where it last matched.
    nx::expect_warning("keeps its last good build");
    edit("    cull = .none\n    sample_count = 4\n");
    auto const kept = slib::configuration_of(definition);
    CHECK(kept.frozen_moved == "sample_count: <unset> -> 4\n");
    CHECK(text_of(kept.settings) == "color_targets.color.format = .host\nrasterization.cull = .front\n");
    // The newest settings are still there, for a host that follows them itself.
    CHECK(text_of(kept.latest) == "color_targets.color.format = .host\nrasterization.cull = .none\nsample_count = 4\n");

    // Taking the frozen part back where it was lets the configuration follow again.
    edit("    cull = .none\n");
    CHECK(text_of(slib::configuration_of(definition).settings)
          == "color_targets.color.format = .host\nrasterization.cull = .none\n");

    // A group the host has no generated type for moves the layout, which is frozen like a format.
    nx::expect_warning("keeps its last good build");
    fs->write("pipeline.sgl", "binding frame:\n    scale: float\n"
                              "@vertex struct vin:\n    p: pos3\n"
                              "struct link:\n    @position p: hpos4\n"
                              "@pixel struct target:\n    color: float4\n"
                              "@vertex fun vs(v: vin){frame} -> link:\n    return { p = hpos4(..v.p, frame.scale) }\n"
                              "@pixel fun ps(l: link) -> target:\n    return { color = float4(1.0, 1.0, 1.0, 1.0) }\n"
                              "pipeline:\n    vertex = vs\n    pixel = ps\n    format = .host\n    cull = .back\n");
    lib.poll_hot_reload();
    compile();
    auto const regrouped = slib::configuration_of(definition);
    CHECK(regrouped.frozen_moved == "layout:  -> frame\n");
    CHECK(text_of(regrouped.settings) == "color_targets.color.format = .host\nrasterization.cull = .none\n");
}


TEST("slib pipeline - a reload promoted before a pipeline's first acquire is read by it",
     exclusive("slib-shader-library"))
{
    auto const fs = std::make_shared<slib::memory_filesystem>();
    fs->write("early.sgl", reload_source(""));

    auto lib = slib::shader_library();
    lib.add_compiler(slib::create_sgl_compiler(slib::create_wgsl_compiler()));
    // Static like a generated package's handles: slib keys a pipeline's reload state by its definition's address.
    static auto vs = slib::shader_asset_handle();
    static auto ps = slib::shader_asset_handle();
    slib::shader_definition const definitions[] = {
        {.path = "early.sgl", .stage = sg::shader_stage::vertex, .entry_point = "vs", .asset = &vs},
        {.path = "early.sgl", .stage = sg::shader_stage::fragment, .entry_point = "ps", .asset = &ps},
    };
    lib.add_package({.name = "early_pkg", .language = slib::shader_language::sgl, .definitions = definitions}, fs);
    lib.start_hot_reload({.unthreaded = true});

    static cc::string_view const targets[] = {"color"};
    static slib::pipeline_setting const baked[] = {
        {.path = "color_targets.color.format", .kind = slib::setting_kind::host},
    };
    static auto const definition = slib::pipeline_definition{.file = "early.sgl",
                                                             .name = "pipeline",
                                                             .vertex = &vs,
                                                             .pixel = &ps,
                                                             .targets = targets,
                                                             .settings = baked,
                                                             .vertex_input_name = "vin",
                                                             .target_struct = "target"};
    auto const compile = [&]
    {
        REQUIRE(vs->acquire(sg::shader_format::wgsl)->has_value());
        REQUIRE(ps->acquire(sg::shader_format::wgsl)->has_value());
    };

    // The stages are acquired, edited and reloaded by someone else before the pipeline is ever asked about.
    compile();
    lib.poll_hot_reload();
    fs->write("early.sgl", reload_source("    cull = .front\n"));
    lib.poll_hot_reload();
    compile();
    REQUIRE(vs->generation() > 0);

    CHECK(text_of(slib::configuration_of(definition).settings)
          == "color_targets.color.format = .host\nrasterization.cull = .front\n");
}
