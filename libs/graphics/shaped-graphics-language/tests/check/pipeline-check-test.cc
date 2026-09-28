#include "check-test-support.hh"

using namespace sgl_test;

namespace
{
/// Two stages that fit, over two targets, and nothing else; each test appends what it is about.
constexpr auto stages
    = cc::string_view("binding frame:\n    scale: float\n"
                      "binding material:\n    tint: float\n"
                      "@inline binding constants:\n    offset: float\n"
                      "@vertex struct vin:\n    p: pos3\n"
                      "struct link:\n    @position p: hpos4\n    n: vec3\n"
                      "@pixel struct gbuffer:\n    albedo: float4\n    normal: float4\n"
                      "@vertex fun vs(v: vin){frame, constants} -> link:\n"
                      "    return { p = hpos4(..v.p, frame.scale), n = vec3(0.0, 1.0, 0.0) }\n"
                      "@pixel fun ps(l: link){frame, material} -> gbuffer:\n"
                      "    return { albedo = float4(..l.n, material.tint), normal = float4(..l.n, 0.0) }\n");

/// The formats every pipeline over `gbuffer` needs, so a test about something else states them once.
constexpr auto formats = cc::string_view("    color_targets.albedo.format = .rgba8_unorm\n"
                                         "    color_targets.normal.format = .rgba16_float\n");

checked_sources checked(cc::string_view program)
{
    return check_sources(read_prelude(), cc::string(stages) + program);
}

cc::string reports(cc::string_view program)
{
    return reports_of(checked(program));
}

/// The settings of the module's one pipeline, in the order they apply, one `path = value` per line.
cc::string settings_of(cc::string_view program)
{
    auto const s = checked(program);
    auto out = reports_of(s);
    if (s.module.pipelines.size() != 1)
        return out + cc::format("{} pipelines\n", s.module.pipelines.size());
    for (auto const& setting : s.module.at(s.module.pipelines[0].settings))
    {
        out.appendf("{} = ", setting.path);
        switch (setting.kind)
        {
        case sgl::check::setting_kind::boolean:
            out += setting.integer != 0 ? "true" : "false";
            break;
        case sgl::check::setting_kind::integer:
            out.appendf("{}", setting.integer);
            break;
        case sgl::check::setting_kind::real:
            out.appendf("{}", setting.real);
            break;
        case sgl::check::setting_kind::enum_case:
            out.appendf(".{}", setting.enum_case);
            break;
        case sgl::check::setting_kind::host:
            out += ".host";
            break;
        case sgl::check::setting_kind::none:
            out += ".none";
            break;
        }
        out += "\n";
    }
    return out;
}
} // namespace

TEST("sgl check - a pipeline names its stages, and its settings are paths into the description")
{
    CHECK(settings_of(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats
                      + "    rasterization.cull = .none\n    depth_stencil.depth_test = true\n")
          == "color_targets.albedo.format = .rgba8_unorm\n"
             "color_targets.normal.format = .rgba16_float\n"
             "rasterization.cull = .none\n"
             "depth_stencil.depth_test = true\n");

    auto const s = checked(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats);
    REQUIRE(s.module.pipelines.size() == 1);
    auto const& p = s.module.pipelines[0];
    // An unnamed pipeline is the file's `pipeline`.
    CHECK(s.module.at(p.symbol).name == "pipeline");
    CHECK(s.module.at(p.vertex).name == "vs");
    CHECK(s.module.at(p.pixel).name == "ps");
    // The layout is the longest list, `@inline` left out, and the one `@inline` binding stands apart.
    auto const layout = s.module.at(p.layout);
    REQUIRE(layout.size() == 2);
    CHECK(s.module.at(layout[0]).name == "frame");
    CHECK(s.module.at(layout[1]).name == "material");
    CHECK(s.module.at(p.inline_constants).name == "constants");
    CHECK(s.module.name_of(p.vertex_input) == "vin");
    CHECK(s.module.name_of(p.target_set) == "gbuffer");
}

TEST("sgl check - a setting's name stands for its path while it is unique")
{
    CHECK(settings_of(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats
                      + "    cull = .back\n    depth_compare = .less_equal\n    depth_bias = -2\n    front = "
                        ".clockwise\n")
          == "color_targets.albedo.format = .rgba8_unorm\n"
             "color_targets.normal.format = .rgba16_float\n"
             "rasterization.cull = .back\n"
             "depth_stencil.depth_compare = .less_equal\n"
             "rasterization.depth_bias = -2\n"
             "rasterization.front = .clockwise\n");

    CHECK(reports(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats + "    compare = .less\n")
          == "invalid-pipeline user:[compare] compare names both depth_stencil.stencil_front.compare and "
             "depth_stencil.stencil_back.compare: write the whole path\n");
    CHECK(reports(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats + "    culling = .back\n")
          == "invalid-pipeline user:[culling] no setting is named culling\n");
    CHECK(reports(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats
                  + "    rasterization.culling = .back\n")
          == "invalid-pipeline user:[culling] rasterization_state has no field culling\n");
    // Only the first name is lifted.
    CHECK(reports(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats + "    stencil_front.cull = .back\n")
          == "invalid-pipeline user:[cull] stencil_face has no field cull\n");
}

TEST("sgl check - a setting under color_targets is every target's, and one target is set by its name")
{
    CHECK(settings_of("pipeline:\n    vertex = vs\n    pixel = ps\n    format = .rgba8_unorm\n"
                      "    color_targets.normal.format = .rgba16_float\n    blend = .none\n")
          == "color_targets.albedo.format = .rgba8_unorm\n"
             "color_targets.normal.format = .rgba8_unorm\n"
             "color_targets.normal.format = .rgba16_float\n"
             "color_targets.albedo.blend = .none\n"
             "color_targets.normal.blend = .none\n");

    CHECK(reports("pipeline:\n    vertex = vs\n    pixel = ps\n    format = .rgba8_unorm\n"
                  "    color_targets.emissive.format = .rgba8_unorm\n")
          == "invalid-pipeline user:[emissive] the pixel stage has no target emissive\n");
    CHECK(reports("pipeline:\n    vertex = vs\n    pixel = ps\n    color_targets = .rgba8_unorm\n")
          == "invalid-pipeline user:[color_targets] a target is set by its member name: "
             "`color_targets.<target>.format`\n");
}

TEST("sgl check - a paren literal replaces a whole part, and names every field of it")
{
    CHECK(settings_of(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats
                      + "    color_targets.albedo.write_mask = (r = true, g = true, b = true, a = false)\n")
          == "color_targets.albedo.format = .rgba8_unorm\n"
             "color_targets.normal.format = .rgba16_float\n"
             "color_targets.albedo.write_mask.r = true\n"
             "color_targets.albedo.write_mask.g = true\n"
             "color_targets.albedo.write_mask.b = true\n"
             "color_targets.albedo.write_mask.a = false\n");

    CHECK(reports(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats
                  + "    stencil_front = (fail = .keep, pass = .replace)\n")
          == "missing-field user:[(fail = .keep, pass = .replace)] depth_fail: a part is written whole, or one field "
             "at a time by its path\n"
             "missing-field user:[(fail = .keep, pass = .replace)] compare: a part is written whole, or one field at a "
             "time by its path\n");
}

TEST("sgl check - a setting's value has the type of its field")
{
    auto const bad = [](cc::string_view line)
    { return reports(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats + line); };
    CHECK(bad("    cull = back\n")
          == "invalid-pipeline user:[back] rasterization.cull is a cull_mode: name a case, as in `.none`\n");
    CHECK(bad("    cull = .sideways\n") == "unknown-member user:[.sideways] the enum cull_mode has no case sideways\n");
    CHECK(bad("    depth_test = 1\n")
          == "invalid-pipeline user:[1] depth_stencil.depth_test is a bool: `true` or `false`\n");
    CHECK(bad("    sample_count = 1.5\n") == "invalid-pipeline user:[1.5] sample_count is an int: write a number\n");
    // A hex literal is a number, which the checker cannot read yet: unsupported, not "write a number".
    CHECK(bad("    stencil_read_mask = 0xFF\n") == "unsupported-yet user:[0xFF] a hex literal\n");
    // `.host` is only for what the host knows better: a format, and the sample count.
    CHECK(bad("    cull = .host\n")
          == "invalid-pipeline user:[.host] rasterization.cull is no format and no sample count, so the host cannot "
             "state it\n");
}

TEST("sgl check - an int setting is held to the range sg keeps it in")
{
    auto const bad = [](cc::string_view line)
    { return reports(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats + line); };
    // A mask is a byte in sg, so 300 would reach it as 44.
    CHECK(bad("    stencil_read_mask = 300\n")
          == "invalid-pipeline user:[300] depth_stencil.stencil_read_mask is a byte: 0 to 255\n");
    CHECK(bad("    stencil_write_mask = -1\n")
          == "invalid-pipeline user:[-1] depth_stencil.stencil_write_mask is a byte: 0 to 255\n");
    CHECK(bad("    sample_count = 3\n") == "invalid-pipeline user:[3] sample_count is a power of two from 1 to 64\n");
    CHECK(bad("    sample_count = 0\n") == "invalid-pipeline user:[0] sample_count is a power of two from 1 to 64\n");
    CHECK(bad("    patch_control_points = 33\n") == "invalid-pipeline user:[33] patch_control_points is 0 to 32\n");
    // The edges are in range.
    CHECK(bad("    stencil_read_mask = 255\n    sample_count = 64\n    patch_control_points = 0\n") == "");
}

TEST("sgl check - every target has a format, stated or left to the host")
{
    CHECK(reports("pipeline:\n    vertex = vs\n    pixel = ps\n    color_targets.albedo.format = .rgba8_unorm\n")
          == "invalid-pipeline user:[pipeline:] the target normal has no format: set `color_targets.normal.format`, or "
             "leave it to the host with `.host`\n");
    CHECK(settings_of("pipeline:\n    vertex = vs\n    pixel = ps\n    format = .host\n    sample_count = .host\n")
          == "color_targets.albedo.format = .host\n"
             "color_targets.normal.format = .host\n"
             "sample_count = .host\n");
}

TEST("sgl check - the stages of a pipeline pass one interface, member for member")
{
    auto const other = cc::string_view("struct other:\n    @position p: hpos4\n    m: vec3\n"
                                       "@pixel fun other_ps(l: other) -> gbuffer:\n"
                                       "    return { albedo = float4(..l.m, 1.0), normal = float4(..l.m, 0.0) }\n");
    CHECK(reports(cc::string(other) + "pipeline:\n    vertex = vs\n    pixel = other_ps\n" + formats)
          == "invalid-pipeline user:[pipeline:] the stages pass one interface, member for member: member 1 is `n: "
             "vec3` where vs returns it and `m: vec3` where other_ps takes it\n");
}

TEST("sgl check - the binding lists of a pipeline's stages agree by position")
{
    auto const swapped = cc::string_view("@pixel fun swapped_ps(l: link){material, frame} -> gbuffer:\n"
                                         "    return { albedo = float4(..l.n, material.tint), normal = float4(..l.n, "
                                         "frame.scale) }\n");
    CHECK(reports(cc::string(swapped) + "pipeline:\n    vertex = vs\n    pixel = swapped_ps\n" + formats)
          == "invalid-pipeline user:[pipeline:] group 0 is frame to one stage and material to another: the binding "
             "lists agree by position\n");

    auto const two_inline = cc::string_view("@inline binding more:\n    extra: float\n"
                                            "@pixel fun inline_ps(l: link){frame, more} -> gbuffer:\n"
                                            "    return { albedo = float4(..l.n, more.extra), normal = float4(..l.n, "
                                            "frame.scale) }\n");
    CHECK(reports(cc::string(two_inline) + "pipeline:\n    vertex = vs\n    pixel = inline_ps\n" + formats)
          == "invalid-pipeline user:[pipeline:] the stages list two @inline bindings, constants and more, and a "
             "pipeline has one\n");
}

TEST("sgl check - attributes on the code are settings too, and the declaration has the last word")
{
    auto const attributed = cc::string_view("@vertex struct avin:\n    p: pos3\n"
                                            "@pixel struct single:\n    @format(.rgba8_unorm) @write_mask((r = true, g "
                                            "= true, b = true, a = false)) c: float4\n"
                                            "@cull(.none) @vertex fun avs(v: avin) -> link:\n"
                                            "    return { p = hpos4(..v.p, 1.0), n = vec3(0.0, 1.0, 0.0) }\n"
                                            "@depth_test(true) @pixel fun aps(l: link) -> single:\n    return { c = "
                                            "float4(..l.n, 1.0) }\n");
    CHECK(settings_of(cc::string(attributed) + "pipeline:\n    vertex = avs\n    pixel = aps\n    depth_write = true\n")
          == "color_targets.c.format = .rgba8_unorm\n"
             "color_targets.c.write_mask.r = true\n"
             "color_targets.c.write_mask.g = true\n"
             "color_targets.c.write_mask.b = true\n"
             "color_targets.c.write_mask.a = false\n"
             "rasterization.cull = .none\n"
             "depth_stencil.depth_test = true\n"
             "depth_stencil.depth_write = true\n");

    // Two stages that disagree are an error, unless the declaration decides it.
    auto const disagreeing
        = cc::string(attributed)
        + "@cull(.back) @pixel fun back_ps(l: link) -> single:\n    return { c = float4(..l.n, 1.0) }\n";
    CHECK(reports(disagreeing + "pipeline:\n    vertex = avs\n    pixel = back_ps\n")
          == "invalid-pipeline user:[cull] rasterization.cull is set differently by two stages; the pipeline sets it "
             "to decide\n");
    CHECK(reports(disagreeing + "pipeline:\n    vertex = avs\n    pixel = back_ps\n    cull = .front\n") == "");

    // `blend = .none` writes the blend itself, so it meets every field another stage's whole blend writes.
    auto const whole = cc::string_view("@blend((color = (source = .one, target = .one, op = .add), alpha = (source = "
                                       ".one, target = .one, op = .add)))");
    auto const blending = cc::string(attributed) + "@blend(.none) @vertex fun none_vs(v: avin) -> link:\n"
                        + "    return { p = hpos4(..v.p, 1.0), n = vec3(0.0, 1.0, 0.0) }\n" + whole
                        + " @pixel fun add_ps(l: link) -> single:\n    return { c = float4(..l.n, 1.0) }\n";
    CHECK(reports(blending + "pipeline:\n    vertex = none_vs\n    pixel = add_ps\n")
          == "invalid-pipeline user:[blend] color_targets.c.blend is set differently by two stages; the pipeline sets "
             "it to decide\n");
    // And the pipeline's own `blend = .none` writes the whole blend, which settles it.
    CHECK(reports(blending + "pipeline:\n    vertex = none_vs\n    pixel = add_ps\n    blend = .none\n") == "");

    // An attribute has no path to name one of two fields by, so an ambiguous one says where to set it instead.
    CHECK(reports("@compare(.less) @pixel fun cmp_ps(l: link) -> single:\n    return { c = float4(..l.n, 1.0) }\n"
                  "@pixel struct single:\n    c: float4\n")
          == "invalid-pipeline user:[compare] @compare names both depth_stencil.stencil_front.compare and "
             "depth_stencil.stencil_back.compare: set it in the pipeline by its whole path\n");

    // An attribute that names no setting is still the compiler's to judge.
    CHECK(reports("@culling(.none) @pixel fun bad_ps(l: link) -> gbuffer:\n"
                  "    return { albedo = float4(..l.n, 1.0), normal = float4(..l.n, 0.0) }\n")
          == "unsupported-yet user:[culling] the attribute @culling on a function\n");
}

TEST("sgl check - the short form places each entry point by its stage")
{
    CHECK(reports("pipeline quick = (ps, vs)\n")
          == "invalid-pipeline user:[pipeline quick = (ps, vs)] the target albedo has no format: set "
             "`color_targets.albedo.format`, or leave it to the host with `.host`\n"
             "invalid-pipeline user:[pipeline quick = (ps, vs)] the target normal has no format: set "
             "`color_targets.normal.format`, or leave it to the host with `.host`\n");
    CHECK(reports("pipeline twice = (vs, vs)\n") == "invalid-pipeline user:[vs] a pipeline has one vertex stage\n");
    // The short form takes the settings a long one would, in a block of its own.
    CHECK(settings_of(cc::string("pipeline quick = (ps, vs):\n    cull = .back\n") + formats)
          == "rasterization.cull = .back\n"
             "color_targets.albedo.format = .rgba8_unorm\n"
             "color_targets.normal.format = .rgba16_float\n");
    CHECK(reports(cc::string("pipeline quick = (ps, vs):\n    vertex = vs\n") + formats)
          == "invalid-pipeline user:[vertex = vs] the short form names its stages in its list\n");
    // The long form holds to the same rule, rather than letting a later stage line override.
    CHECK(reports(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n    vertex = vs\n") + formats)
          == "invalid-pipeline user:[vs] a pipeline has one vertex stage\n");
    CHECK(reports("pipeline:\n    pixel = ps\n")
          == "invalid-pipeline user:[pipeline:] a pipeline has a vertex stage: `vertex = <entry point>`\n");
    CHECK(reports("pipeline:\n    vertex = ps\n") == "invalid-pipeline user:[ps] ps is no @vertex entry point\n");
    CHECK(reports("fun helper() -> float => 1.0\npipeline:\n    vertex = helper\n")
          == "invalid-pipeline user:[helper] helper is no entry point\n");
}

TEST("sgl check - a target is not named like a setting of the pipeline itself")
{
    // The host states an open target's format by the target's name, beside `sample_count` and `depth_stencil_format`.
    CHECK(reports("@pixel struct odd:\n    sample_count: float4\n"
                  "@pixel fun odd_ps(l: link) -> odd:\n    return { sample_count = float4(..l.n, 1.0) }\n"
                  "pipeline:\n    vertex = vs\n    pixel = odd_ps\n    format = .host\n")
          == "invalid-pipeline user:[pipeline:] a target of a pipeline is not named sample_count, which the pipeline's "
             "own setting is\n");
}

TEST("sgl check - a pipeline shares its file's names, and only a raster pipeline is declared")
{
    CHECK(reports(cc::string("pipeline:\n    vertex = vs\n    pixel = ps\n") + formats + "pipeline:\n    vertex = vs\n")
          == "duplicate-declaration user:[pipeline:] pipeline\n");
    CHECK(reports(cc::string("pipeline vs:\n    vertex = vs\n    pixel = ps\n") + formats)
          == "duplicate-declaration user:[vs] vs\n");
    CHECK(reports("@compute pipeline p = (vs, ps)\n")
          == "unsupported-yet user:[compute] a @compute pipeline; every compute entry point is its own\n");
}
