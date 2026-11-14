#include "emit-test-support.hh"

#include <babel-data/shader/dxil_container.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::check::core_construct;
using sgl::check::dxil_shader_model;
using sgl::check::feature;
using sgl::check::feature_set;
using sgl::check::fill;
using sgl::emit::native_route;
using sgl::emit::target;

// What a dx12 output needs of the shader model (EMIT-264), by the fill a compile asks for each feature and core
// construct (EMIT-265), and the variants an entry point's consults make (EMIT-266).

namespace
{
constexpr auto k_draws = "struct varyings:\n"
                         "    @position position: hpos4\n"
                         "\n"
                         "@vertex fun vs(@vertex_index v: int, @instance_index i: int) -> varyings:\n"
                         "    return { position = hpos4(v as float, i as float, 0.0, 1.0) }\n";

constexpr auto k_plain = "binding work:\n"
                         "    values: mut buffer[float]\n"
                         "\n"
                         "struct varyings:\n"
                         "    @position position: hpos4\n"
                         "\n"
                         "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                         "    work.values[id.x] = 1.0\n"
                         "\n"
                         "@pixel fun ps(p: varyings) -> float4:\n"
                         "    return float4(p.position.x, 0.0, 0.0, 1.0)\n";

/// The stages the repo's shaders hold none of, which the floor sweep therefore never meets.
constexpr auto k_primitive_stages = cc::string_view(
    "require geometry_shader\n"
    "require tessellation_shader\n"
    "binding params:\n    level: float\n"
    "struct control_point:\n    position: float3\n"
    "struct varyings:\n    @position position: hpos4\n    color: float3\n"
    "struct tri_factors:\n"
    "    @edge_factors edges: float[3]\n"
    "    @inside_factors inside: float\n"
    "    bulge: float\n"
    "@tessellation_control(partitioning = .fractional_odd, winding = .counter_clockwise)\n"
    "fun tc(patch: control_point[3]){params} -> tri_factors:\n"
    "    return { edges = [params.level, params.level, params.level], inside = params.level, bulge = 0.0 }\n"
    "@tessellation_evaluation\n"
    "fun te(patch: control_point[3], f: tri_factors, @domain_location uvw: float3) -> varyings:\n"
    "    let p = patch[0].position * uvw.x + patch[1].position * uvw.y + patch[2].position * uvw.z\n"
    "    return { position = hpos4(p.x, p.y, p.z + f.bulge, 1.0), color = uvw }\n"
    "@geometry(max_vertices = 6)\n"
    "fun gs(tri: varyings[3], @primitive_id prim: int, stream: mut triangle_stream[varyings]):\n"
    "    for i in 0 ..< 3:\n"
    "        stream.emit(tri[i])\n"
    "    stream.end_strip()\n");

/// An entry point that consults the device by `has`.
constexpr auto k_consulting = "binding work:\n"
                              "    values: mut buffer[float]\n"
                              "\n"
                              "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                              "    const if has(.subgroups):\n"
                              "        work.values[id.x] = 1.0\n"
                              "    else:\n"
                              "        work.values[id.x] = 2.0\n";

/// A raygen that reorders as it traces, and traces a hit object it reads and regroups by, where the device can.
constexpr auto k_reordering
    = "require raytracing_pipeline\n"
      "binding frame:\n"
      "    world: acceleration_structure[.triangles]\n"
      "    seen: mut buffer[float]\n"
      "struct radiance:\n"
      "    color: float3\n"
      "rays path_rays:\n"
      "    surface: radiance\n"
      "@raygen fun shoot(@launch_id id: int3){frame}:\n"
      "    let mut p = radiance(float3(0.0, 0.0, 0.0))\n"
      "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
      "    trace(frame.world, r, path_rays.surface, mut p, reorder = id.x as uint, reorder_bits = 4)\n"
      "    const if has(.shader_execution_reordering):\n"
      "        let h = trace_hit(frame.world, r, path_rays.surface, mut p)\n"
      "        if h.is_hit and h.t > 0.5 => p.color = float3(h.instance_id as float, 0.0, 0.0)\n"
      "        if h.is_miss => p.color.y = (h.primitive_index as float) + (h.geometry_index as float)\n"
      "        h.reorder(h.instance_index as uint, 4)\n"
      "        h.reorder()\n"
      "    frame.seen[id.x] = p.color.x + p.color.y\n"
      "@miss fun on_miss(p: mut radiance):\n"
      "    p.color = float3(1.0, 0.0, 0.0)\n"
      "@closest_hit fun on_hit(h: triangle_hit, p: mut radiance):\n"
      "    p.color = float3(0.0, 1.0, 0.0)\n"
      "hit_group hits for path_rays:\n"
      "    surface = (closest_hit = on_hit)\n"
      "@raytracing pipeline path:\n"
      "    rays = path_rays\n"
      "    raygen = shoot\n"
      "    miss.surface = on_miss\n"
      "    hit_groups = (hits)\n";

/// A ray query, which a dx12 compile states sees opacity micromaps where the device has them (EMIT-184).
constexpr auto k_micromap_query
    = "require ray_query\n"
      "\n"
      "binding work:\n"
      "    world: acceleration_structure[.triangles]\n"
      "    values: mut buffer[float]\n"
      "\n"
      "@compute(64) fun cs(@thread_id id: int3){work}:\n"
      "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
      "    let flags = ray_flags.force_omm_2_state\n"
      "    work.values[id.x] = work.world.trace(r, c => hit_decision.ignore, flags = flags).t\n";

/// The same query forcing nothing, which sees whatever micromaps its structure has.
constexpr auto k_micromap_seen = "require ray_query\n"
                                 "\n"
                                 "binding work:\n"
                                 "    world: acceleration_structure[.triangles]\n"
                                 "    values: mut buffer[float]\n"
                                 "\n"
                                 "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                                 "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
                                 "    work.values[id.x] = work.world.trace(r, c => hit_decision.ignore).t\n";

/// `source`'s `entry` on dx12, for a device with `f`, which the compile fills as `how`.
[[nodiscard]] cc::result<sgl::emitted_source, cc::string> filled(cc::string_view source,
                                                                 cc::string_view entry,
                                                                 feature f,
                                                                 fill how,
                                                                 native_route route)
{
    sgl::check::feature_fill const fills[] = {{.f = f, .how = how}};
    return sgl::compile_to_text({.source = source,
                                 .entry_point = entry,
                                 .target = target::hlsl_dx12,
                                 .device_features = f,
                                 .fills = fills,
                                 .native = route});
}

constexpr native_route k_routes[] = {native_route::text, native_route::require};

constexpr auto k_six_six = dxil_shader_model{.major = 6, .minor = 6};
constexpr auto k_six_eight = dxil_shader_model{.major = 6, .minor = 8};
/// The core fills a host compiling dx12 for Shader Model 6.6 asks for.
constexpr auto k_six_six_fills = sgl::check::core_fills{.draw_start = fill::root_constant, .wave_size = fill::none};

/// The shader model SGL's own DXIL container states in its program's header.
[[nodiscard]] dxil_shader_model container_model(cc::span<byte const> bytes)
{
    auto const c = babel::dxil::read(bytes);
    REQUIRE(c.has_value());
    auto const* const part = c.value().part_named("DXIL");
    REQUIRE(part != nullptr);
    auto const p = babel::dxil::program_of(*part);
    REQUIRE(p.has_value());
    return {.major = i32(p.value().model_major), .minor = i32(p.value().model_minor)};
}

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> dx12(cc::string_view source,
                                                               cc::string_view entry,
                                                               native_route route,
                                                               sgl::check::core_fills core = {})
{
    return sgl::compile_to_text(
        {.source = source, .entry_point = entry, .target = target::hlsl_dx12, .core = core, .native = route});
}

[[nodiscard]] bool same_fills(cc::span<fill const> a, std::initializer_list<fill> b)
{
    if (a.size() != isize(b.size()))
        return false;
    auto i = isize(0);
    for (auto const f : b)
        if (a[i++] != f)
            return false;
    return true;
}

[[nodiscard]] bool ast_equal_axes(cc::span<sgl::variant_axis const> a, cc::span<sgl::variant_axis const> b)
{
    if (a.size() != b.size())
        return false;
    for (auto i = isize(0); i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}
} // namespace

TEST("sgl emit - each feature and core construct names the DXIL shader model its fill needs (EMIT-264)")
{
    CHECK(sgl::check::dxil_shader_model_of(feature::shader_execution_reordering) == dxil_shader_model{.minor = 9});
    CHECK(sgl::check::dxil_shader_model_of(feature::shader_execution_reordering, fill::nvapi) == k_six_six);
    CHECK(sgl::check::dxil_shader_model_of(feature::opacity_micromap) == dxil_shader_model{.minor = 9});
    CHECK(sgl::check::dxil_shader_model_of(feature::opacity_micromap, fill::nvapi) == k_six_six);
    CHECK(sgl::check::dxil_shader_model_of(feature::clustered_geometry) == dxil_shader_model{.minor = 10});
    // what 6.6 already has needs nothing more; `@first_instance`'s model is the draw start's fill
    for (auto const f : {feature::ray_query, feature::raytracing_pipeline, feature::mesh_shader, feature::subgroups,
                         feature::helper_lane_query, feature::shader_int64_atomics, feature::first_instance})
        CHECK(sgl::check::dxil_shader_model_of(f) == k_six_six).dump("feature", sgl::check::name_of(f));
    CHECK(sgl::check::dxil_shader_model_of(core_construct::draw_start, fill::native) == k_six_eight);
    CHECK(sgl::check::dxil_shader_model_of(core_construct::draw_start, fill::root_constant) == k_six_six);
    CHECK(sgl::check::dxil_shader_model_of(core_construct::wave_size, fill::native) == k_six_eight);
    CHECK(sgl::check::dxil_shader_model_of(core_construct::wave_size, fill::none) == k_six_six);
    CHECK(k_six_six < k_six_eight);
    CHECK((dxil_shader_model{.minor = 10} < dxil_shader_model{.major = 7, .minor = 0}));
}

TEST("sgl emit - each target offers its fills, most demanding first (EMIT-265)")
{
    CHECK(same_fills(sgl::emit::fills_of(target::hlsl_dx12, feature::shader_execution_reordering),
                     {fill::native, fill::nvapi}));
    CHECK(same_fills(sgl::emit::fills_of(target::hlsl_dx12, feature::opacity_micromap), {fill::native, fill::nvapi}));
    CHECK(same_fills(sgl::emit::fills_of(target::hlsl_dx12, feature::subgroups), {fill::native}));
    CHECK(same_fills(sgl::emit::fills_of(target::hlsl_vulkan, feature::shader_execution_reordering), {fill::native}));
    CHECK(same_fills(sgl::emit::fills_of(target::hlsl_dx12, core_construct::draw_start),
                     {fill::native, fill::root_constant}));
    CHECK(same_fills(sgl::emit::fills_of(target::hlsl_dx12, core_construct::wave_size), {fill::native, fill::none}));
    for (auto const t : {target::hlsl_vulkan, target::wgsl, target::msl})
        for (auto const c : {core_construct::draw_start, core_construct::wave_size})
            CHECK(same_fills(sgl::emit::fills_of(t, c), {fill::native}));

    // a fill the target does not offer is no output of it
    sgl::check::feature_fill const nvapi[] = {{.f = feature::shader_execution_reordering, .how = fill::nvapi}};
    auto const vulkan = sgl::compile_to_text({.source = k_plain,
                                              .entry_point = "cs",
                                              .target = target::hlsl_vulkan,
                                              .device_features = feature::shader_execution_reordering,
                                              .fills = nvapi});
    REQUIRE(vulkan.has_error());
    CHECK(vulkan.error().contains("a nvapi fill of shader_execution_reordering, which hlsl-vulkan does not offer"));
    auto const metal = sgl::compile_to_text(
        {.source = k_plain, .entry_point = "cs", .target = target::msl, .core = {.wave_size = fill::none}});
    REQUIRE(metal.has_error());
    CHECK(metal.error().contains("a none fill of wave_size, which msl does not offer"));
}

TEST("sgl emit - the root-constant fill of the draw's start refuses an entry that takes it, for now (EMIT-128)")
{
    for (auto const route : k_routes)
    {
        auto const refused = dx12(k_draws, "vs", route, k_six_six_fills);
        REQUIRE(refused.has_error());
        CHECK(refused.error()
              == "<sgl>: error: unsupported: vs takes @vertex_index, which dx12 reads natively from Shader Model 6.8's "
                 "system values, and this compile asks the root-constant fill of draw_start, which is not written "
                 "yet\n");

        auto const instance = dx12("struct varyings:\n"
                                   "    @position position: hpos4\n"
                                   "\n"
                                   "@vertex fun vs(@instance_index i: int) -> varyings:\n"
                                   "    return { position = hpos4(i as float, 0.0, 0.0, 1.0) }\n",
                                   "vs", route, k_six_six_fills);
        REQUIRE(instance.has_error());
        CHECK(instance.error().contains("vs takes @instance_index, which dx12 reads natively"));

        // `@first_instance` is the draw's start itself
        auto const first = dx12("require first_instance\n"
                                "struct varyings:\n"
                                "    @position position: hpos4\n"
                                "\n"
                                "@vertex fun vs(@first_instance f: int) -> varyings:\n"
                                "    return { position = hpos4(f as float, 0.0, 0.0, 1.0) }\n",
                                "vs", route, k_six_six_fills);
        REQUIRE(first.has_error());
        CHECK(first.error().contains("vs takes @first_instance, which dx12 reads natively"));
    }
    // vulkan reads the draw's start as its builtins under its one fill
    auto const vulkan = sgl::compile_to_text({.source = k_draws, .entry_point = "vs", .target = target::hlsl_vulkan});
    REQUIRE(vulkan.has_value());
    CHECK(vulkan.value().text.contains("[[vk::builtin(\"BaseVertex\")]]"));
}

TEST("sgl emit - the native fill of the draw's start reads it as system values and states 6.8, the default (EMIT-264)")
{
    for (auto const route : k_routes)
    {
        auto const e = dx12(k_draws, "vs", route);
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(e.value().dxil_shader_model == k_six_eight);
        if (route == native_route::text)
            CHECK(e.value().text.contains("SV_StartVertexLocation"));
        else
            CHECK(container_model(e.value().dxil) == k_six_eight);
    }
}

TEST("sgl emit - a dx12 entry that reads no draw start stays at 6.6 under either core fill (EMIT-264)")
{
    for (auto const route : k_routes)
        for (auto const entry : {cc::string_view("cs"), cc::string_view("ps")})
            for (auto const core : {sgl::check::core_fills(), k_six_six_fills})
            {
                auto const e = dx12(k_plain, entry, route, core);
                REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
                CHECK(e.value().dxil_shader_model == k_six_six).dump("entry", entry);
                CHECK(e.value().dxc_args.empty()).dump("entry", entry);
                if (route == native_route::require)
                    CHECK(container_model(e.value().dxil) == k_six_six).dump("entry", entry);
            }
}

TEST("sgl emit - a dx12 geometry or tessellation stage stays at 6.6 under the 6.6 fills (EMIT-264)")
{
    for (auto const entry : {cc::string_view("tc"), cc::string_view("te"), cc::string_view("gs")})
        for (auto const route : {native_route::text, native_route::prefer})
        {
            auto const e = dx12(k_primitive_stages, entry, route, k_six_six_fills);
            REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
            CHECK(e.value().dxil_shader_model == k_six_six).dump("entry", entry);
            if (!e.value().dxil.empty())
                CHECK(container_model(e.value().dxil) == k_six_six).dump("entry", entry);
        }
}

TEST("sgl emit - an entry's axes are the features it consults, granted or not, with each target's fills (EMIT-266)")
{
    for (auto const device : {feature_set(), feature_set(feature::subgroups)})
        for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl})
        {
            auto const e = sgl::compile_to_text(
                {.source = k_consulting, .entry_point = "cs", .target = t, .device_features = device});
            REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
            REQUIRE(e.value().axes.size() == 1);
            CHECK(e.value().axes[0].f == feature::subgroups);
            CHECK(same_fills(e.value().axes[0].fills, {fill::native}));
        }
    // an entry that consults nothing has no axis
    auto const plain = dx12(k_plain, "cs", native_route::text);
    REQUIRE(plain.has_value());
    CHECK(plain.value().axes.empty());
}

TEST("sgl emit - an entry's axes reach through calls and generics, in feature order, and only from that entry "
     "(EMIT-266)")
{
    // `has` in a function the entry calls, in a generic one, and in one only another entry calls, written out of order
    constexpr auto source = "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "fun with_int64() -> float:\n"
                            "    const if has(.shader_int64):\n"
                            "        return 1.0\n"
                            "    return 2.0\n"
                            "fun scaled[T](x: T) -> T:\n"
                            "    const if has(.subgroups):\n"
                            "        return x\n"
                            "    return x + x\n"
                            "fun with_f16() -> float:\n"
                            "    const if has(.shader_f16):\n"
                            "        return 1.0\n"
                            "    return 3.0\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    work.values[id.x] = with_int64() + scaled(1.0)\n"
                            "@compute(64) fun other(@thread_id id: int3){work}:\n"
                            "    work.values[id.x] = with_f16()\n";
    for (auto const device : {feature_set(), feature_set(feature::subgroups) | feature::shader_int64})
    {
        auto const e = sgl::compile_to_text(
            {.source = source, .entry_point = "cs", .target = target::hlsl_dx12, .device_features = device});
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        REQUIRE(e.value().axes.size() == 2);
        // feature order, not the order the source asks in
        CHECK(e.value().axes[0].f == feature::subgroups);
        CHECK(e.value().axes[1].f == feature::shader_int64);
        // and the same list on a second compile
        auto const again = sgl::compile_to_text(
            {.source = source, .entry_point = "cs", .target = target::hlsl_dx12, .device_features = device});
        REQUIRE(again.has_value());
        CHECK(ast_equal_axes(again.value().axes, e.value().axes));
    }
}

TEST("sgl emit - a variant's key names its granted features with their fills, then the core fills that are not native")
{
    CHECK(sgl::variant_key().to_string() == "");
    sgl::check::feature_fill const out_of_order[] = {{.f = feature::opacity_micromap, .how = fill::nvapi},
                                                     {.f = feature::shader_execution_reordering, .how = fill::native}};
    auto const key = sgl::variant_key::of(out_of_order, k_six_six_fills);
    REQUIRE(key.has_value());
    CHECK(key.value().to_string()
          == "shader_execution_reordering=native;opacity_micromap=nvapi;draw_start=root_constant;wave_size=none");
    CHECK(key.value().granted() == (feature::shader_execution_reordering | feature::opacity_micromap));
    // canonical: its fills in feature order, so the same variant named in another order is the same key
    REQUIRE(key.value().fills().size() == 2);
    CHECK(key.value().fills()[0].f == feature::shader_execution_reordering);
    sgl::check::feature_fill const in_order[] = {out_of_order[1], out_of_order[0]};
    auto const same = sgl::variant_key::of(in_order, k_six_six_fills);
    REQUIRE(same.has_value());
    CHECK(same.value() == key.value());
    CHECK(!(same.value() == sgl::variant_key::of(in_order).value()));
    // a feature named twice names no variant
    sgl::check::feature_fill const twice[] = {out_of_order[0], {.f = feature::opacity_micromap, .how = fill::native}};
    auto const refused = sgl::variant_key::of(twice);
    REQUIRE(refused.has_error());
    CHECK(refused.error() == "a variant names opacity_micromap twice");
    auto const wave = sgl::variant_key::of({}, {.wave_size = fill::none});
    REQUIRE(wave.has_value());
    CHECK(wave.value().to_string() == "wave_size=none");
}

TEST("sgl emit - a fill of a feature the compile does not grant, or two of one, is the request's error (EMIT-265)")
{
    sgl::check::feature_fill const nvapi[] = {{.f = feature::shader_execution_reordering, .how = fill::nvapi}};
    auto const ungranted = sgl::compile_to_text(
        {.source = k_reordering, .entry_point = "shoot", .target = target::hlsl_dx12, .fills = nvapi});
    REQUIRE(ungranted.has_error());
    CHECK(ungranted.error().contains("a nvapi fill of shader_execution_reordering, which the compile does not grant"));
    sgl::check::feature_fill const twice[] = {nvapi[0], {.f = feature::shader_execution_reordering, .how = fill::native}};
    auto const doubled = sgl::compile_to_text({.source = k_reordering,
                                               .entry_point = "shoot",
                                               .target = target::hlsl_dx12,
                                               .device_features = feature::shader_execution_reordering,
                                               .fills = twice});
    REQUIRE(doubled.has_error());
    CHECK(doubled.error().contains("two fills of shader_execution_reordering"));
}

TEST("sgl emit - every target's compile of a source takes the fills it offers, and native for the rest (EMIT-265)")
{
    sgl::check::feature_fill const nvapi[] = {{.f = feature::shader_execution_reordering, .how = fill::nvapi}};
    auto const all = sgl::compile_all_to_text({.source = k_reordering,
                                               .targets = sgl::emit::all_targets(),
                                               .device_features = feature::shader_execution_reordering,
                                               .fills = nvapi,
                                               .core = k_six_six_fills});
    REQUIRE(all.has_value()).dump("error", all.has_error() ? all.error() : cc::string());
    auto seen = 0;
    for (auto const& entry : all.value())
    {
        if (entry.entry_point != "shoot" || !entry.text.has_value())
            continue;
        ++seen;
        auto const& text = entry.text.value().text;
        if (entry.target == target::hlsl_dx12)
        {
            CHECK(text.contains("NvHitObject"));
            CHECK(entry.text.value().dxil_shader_model == k_six_six);
        }
        else
            CHECK(!text.contains("NvHitObject")).dump("target", sgl::emit::to_string(entry.target));
    }
    CHECK(seen >= 2);
    // a fill no target of the request offers is its error
    target const vulkan_only[] = {target::hlsl_vulkan};
    auto const refused = sgl::compile_all_to_text({.source = k_reordering,
                                                   .targets = vulkan_only,
                                                   .device_features = feature::shader_execution_reordering,
                                                   .fills = nvapi});
    REQUIRE(refused.has_error());
    CHECK(refused.error().contains("a nvapi fill of shader_execution_reordering, which none of the targets offers"));
    // a fill of a feature not granted, and two of one, are the request's own error, whichever targets offer them
    target const both[] = {target::hlsl_vulkan, target::hlsl_dx12};
    auto const ungranted = sgl::compile_all_to_text({.source = k_reordering, .targets = both, .fills = nvapi});
    REQUIRE(ungranted.has_error());
    CHECK(ungranted.error()
          == "<sgl>: error: unsupported: a nvapi fill of shader_execution_reordering, which the compile does not "
             "grant\n");
    sgl::check::feature_fill const twice[] = {nvapi[0], {.f = feature::shader_execution_reordering, .how = fill::native}};
    auto const doubled = sgl::compile_all_to_text(
        {.source = k_reordering, .targets = both, .device_features = feature::shader_execution_reordering, .fills = twice});
    REQUIRE(doubled.has_error());
    CHECK(doubled.error() == "<sgl>: error: unsupported: two fills of shader_execution_reordering\n");
}

TEST("sgl emit - an entry's axes are every has() it can reach, through branches its compile does not take (EMIT-266)")
{
    // nested under a branch this compile does not take, in either order, and in a value if
    constexpr auto nested = "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "fun pick() -> float:\n"
                            "    return if has(.shader_f64) => 1.0 else 2.0\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    const if has(.subgroups):\n"
                            "        const if has(.shader_int64):\n"
                            "            work.values[id.x] = 1.0\n"
                            "    else if has(.shader_int16):\n"
                            "        work.values[id.x] = 2.0\n"
                            "    else:\n"
                            "        const if has(.helper_lane_query):\n"
                            "            work.values[id.x] = pick()\n";
    auto const features_of = [](cc::span<sgl::variant_axis const> axes)
    {
        auto result = feature_set();
        for (auto const& a : axes)
            result.set(a.f);
        return result;
    };
    auto const expected = feature::subgroups | feature::shader_int64 | feature::shader_int16
                        | feature::helper_lane_query | feature::shader_f64;
    for (auto const device : {feature_set(), feature_set(feature::subgroups),
                              feature::subgroups | feature::shader_int64, feature_set(feature::helper_lane_query)})
    {
        auto const e = sgl::compile_to_text(
            {.source = nested, .entry_point = "cs", .target = target::hlsl_dx12, .device_features = device});
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(features_of(e.value().axes) == expected);
    }
    // a preamble's condition varies the text, so it is an axis of every entry that writes a body of its file
    constexpr auto preambled = "const if has(.subgroups):\n"
                               "    \"hlsl\n"
                               "        float with_subgroups(float x);\n"
                               "    \"\n"
                               "\n"
                               "binding work:\n"
                               "    values: mut buffer[float]\n"
                               "\n"
                               "fun twice(x: float) -> float:\n"
                               "    \"hlsl\n"
                               "        $result = $x * 2.0;\n"
                               "    \"\n"
                               "    \"wgsl\n"
                               "        $result = $x * 2.0;\n"
                               "    \"\n"
                               "    \"msl\n"
                               "        $result = $x * 2.0;\n"
                               "    \"\n"
                               "\n"
                               "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                               "    work.values[id.x] = twice(work.values[id.x])\n";
    auto const with = sgl::compile_to_text({.source = preambled, .entry_point = "cs", .target = target::hlsl_dx12});
    REQUIRE(with.has_value()).dump("error", with.has_error() ? with.error() : cc::string());
    CHECK(features_of(with.value().axes) == feature_set(feature::subgroups));
    // an entry that writes no body of that file has no such axis
    auto const plain = sgl::compile_to_text({.source = k_plain, .entry_point = "cs", .target = target::hlsl_dx12});
    REQUIRE(plain.has_value());
    CHECK(plain.value().axes.empty());
}

TEST("sgl emit - a side no grant can take on this target is no axis, and one a grant can take is (EMIT-266)")
{
    auto const axes_of = [](cc::string_view source, target t)
    {
        auto const e = sgl::compile_to_text({.source = source, .entry_point = "cs", .target = t});
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        auto result = feature_set();
        for (auto const& a : e.value().axes)
            result.set(a.f);
        return result;
    };
    // a branch only another target takes
    constexpr auto by_api = "binding work:\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    const if target_api == .vulkan:\n"
                            "        const if has(.shader_int64):\n"
                            "            work.values[id.x] = 1.0\n"
                            "    work.values[id.x] += 2.0\n";
    CHECK(axes_of(by_api, target::hlsl_dx12).is_empty());
    CHECK(axes_of(by_api, target::msl).is_empty());
    CHECK(axes_of(by_api, target::hlsl_vulkan) == feature_set(feature::shader_int64));
    // a branch a generic's argument decides
    constexpr auto by_argument = "binding work:\n"
                                 "    values: mut buffer[float]\n"
                                 "\n"
                                 "fun widened[n: int](x: float) -> float:\n"
                                 "    const if n > 2:\n"
                                 "        const if has(.shader_int64):\n"
                                 "            return x * 2.0\n"
                                 "    return x\n"
                                 "\n"
                                 "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                                 "    work.values[id.x] = widened[1](work.values[id.x])\n";
    CHECK(axes_of(by_argument, target::hlsl_dx12).is_empty());
    // what a grant decides is a variant however it is spelled, and so is what it decides inside
    constexpr auto by_grant = "binding work:\n"
                              "    values: mut buffer[float]\n"
                              "\n"
                              "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                              "    const if target_api == .dx12 and has(.subgroups):\n"
                              "        const if has(.shader_int64):\n"
                              "            work.values[id.x] = 1.0\n"
                              "    work.values[id.x] += 2.0\n";
    CHECK(axes_of(by_grant, target::hlsl_dx12) == (feature::subgroups | feature::shader_int64));
    // and where the target alone already decides it, the grant inside it is no variant
    CHECK(!axes_of(by_grant, target::hlsl_vulkan).has(feature::shader_int64));
}

TEST("sgl emit - a has() a constant decision folded away inside a condition still makes its side an axis (EMIT-266)")
{
    // each condition consults subgroups through a decision of its own, which leaves no has() in the condition itself
    struct folded_case
    {
        cc::string_view what;
        cc::string_view declarations;
        cc::string_view condition;
        /// `const if`, or a plain `if` where the condition is a call that only a plain `if` takes, once inlined, as
        /// a constant: a body of a `const if` and two returns is no constant a `const if` takes (CHK-425).
        cc::string_view keyword = "const if";
    };
    folded_case const cases[] = {
        {"a value if", "", "(if has(.subgroups) => true else false)"},
        {"a callee's value if", "fun can() -> bool:\n    return if has(.subgroups) => true else false\n", "can()"},
        {"a callee's const if",
         "fun can() -> bool:\n    const if has(.subgroups):\n        return true\n    return false\n", "can()", "if"},
    };
    for (auto const& c : cases)
    {
        auto const source = cc::format("binding work:\n"
                                       "    values: mut buffer[float]\n"
                                       "\n"
                                       "{}"
                                       "@compute(64) fun cs(@thread_id id: int3){{work}}:\n"
                                       "    {} {}:\n"
                                       "        const if has(.shader_int64):\n"
                                       "            work.values[id.x] = 1.0\n"
                                       "    work.values[id.x] += 2.0\n",
                                       c.declarations, c.keyword, c.condition);
        // a device without subgroups takes neither side's inside, and both are variants
        for (auto const device : {feature_set(), feature_set(feature::subgroups)})
        {
            auto const e = sgl::compile_to_text(
                {.source = source, .entry_point = "cs", .target = target::hlsl_dx12, .device_features = device});
            REQUIRE(e.has_value()).dump("case", c.what).dump("error", e.has_error() ? e.error() : cc::string());
            auto axes = feature_set();
            for (auto const& a : e.value().axes)
                axes.set(a.f);
            CHECK(axes == (feature::subgroups | feature::shader_int64)).dump("case", c.what);
        }
    }
}

TEST("sgl emit - a has() passed as an argument still makes the side its parameter decides an axis (EMIT-266)")
{
    // the parameter stands for a copy of the has() literal, which no grant flips, while the argument's own literal
    // was flattened before the condition the callee decides
    struct passed_case
    {
        cc::string_view what;
        cc::string_view callee;
    };
    passed_case const cases[] = {
        {"a const if of the parameter", "fun more(fast: bool) -> float:\n"
                                        "    const if fast:\n"
                                        "        const if has(.shader_int64):\n"
                                        "            return 1.0\n"
                                        "    return 0.0\n"},
        {"a value if of the parameter", "fun more(fast: bool) -> float:\n"
                                        "    return if fast => (if has(.shader_int64) => 1.0 else 0.5) else 0.0\n"},
    };
    for (auto const& c : cases)
    {
        auto const source = cc::format("binding work:\n"
                                       "    values: mut buffer[float]\n"
                                       "\n"
                                       "{}"
                                       "\n"
                                       "@compute(64) fun cs(@thread_id id: int3){{work}}:\n"
                                       "    work.values[id.x] = more(has(.subgroups))\n",
                                       c.callee);
        for (auto const device : {feature_set(), feature_set(feature::subgroups)})
        {
            auto const e = sgl::compile_to_text(
                {.source = source, .entry_point = "cs", .target = target::hlsl_dx12, .device_features = device});
            REQUIRE(e.has_value()).dump("case", c.what).dump("error", e.has_error() ? e.error() : cc::string());
            auto axes = feature_set();
            for (auto const& a : e.value().axes)
                axes.set(a.f);
            CHECK(axes == (feature::subgroups | feature::shader_int64))
                .dump("case", c.what)
                .dump("subgroups granted", device.has(feature::subgroups));
        }
    }
}

TEST("sgl emit - the nvapi fill of opacity micromaps traces constant flags that force no 2 states (EMIT-184)")
{
    // flags a trace writes as a constant, however it spells them, force nothing, and the nvapi fill traces them
    struct spelled
    {
        cc::string_view what;
        cc::string_view body;
    };
    spelled const cases[] = {
        {"a combination held in a let", "    let flags = ray_flags.force_opaque | ray_flags.cull_back_facing\n"
                                        "    work.values[id.x] = work.world.trace(r, c => hit_decision.ignore, flags = "
                                        "flags).t\n"},
        {"a combination as the argument", "    work.values[id.x] = work.world.trace(r, c => hit_decision.ignore, flags "
                                          "= "
                                          "ray_flags.force_opaque | ray_flags.cull_back_facing).t\n"},
        {"two traces with flags of their own",
         "    let a = work.world.trace(r, c => hit_decision.ignore, flags = ray_flags.force_opaque).t\n"
         "    let b = work.world.trace(r, c => hit_decision.ignore, flags = ray_flags.cull_back_facing).t\n"
         "    work.values[id.x] = a + b\n"},
        // shaped-viewer's raster_bound_indirect.sgl: a callee's value if of a parameter each call hands a literal
        {"a value if of a literal argument",
         "    work.values[id.x] = traced(r, true) + traced(r, false)\n"
         "\n"
         "fun traced(r: ray, any: bool){work} -> float:\n"
         "    let flags = if any => ray_flags.accept_first_hit_and_end_search else ray_flags.none\n"
         "    return work.world.trace(r, c => hit_decision.ignore, flags = flags).t\n"},
    };
    for (auto const& c : cases)
    {
        auto const source
            = cc::format("require ray_query\n"
                         "\n"
                         "binding work:\n"
                         "    world: acceleration_structure[.triangles]\n"
                         "    values: mut buffer[float]\n"
                         "\n"
                         "@compute(64) fun cs(@thread_id id: int3){{work}}:\n"
                         "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
                         "{}",
                         c.body);
        for (auto const route : k_routes)
        {
            auto const e = filled(source, "cs", feature::opacity_micromap, fill::nvapi, route);
            CHECK(e.has_value())
                .dump("case", c.what)
                .dump("route", route == native_route::text ? "text" : "dxil")
                .dump("error", e.has_error() ? e.error() : cc::string());
        }
    }
}

TEST("sgl emit - the nvapi fill of opacity micromaps refuses flags that are no constant on both routes, and only it "
     "(EMIT-184)")
{
    // flags no constant decides, though neither value forces 2 states
    constexpr auto chosen = "require ray_query\n"
                            "\n"
                            "binding work:\n"
                            "    world: acceleration_structure[.triangles]\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
                            "    let flags = if id.x > 0 => ray_flags.force_opaque else ray_flags.none\n"
                            "    work.values[id.x] = work.world.trace(r, c => hit_decision.ignore, flags = flags).t\n";
    for (auto const route : k_routes)
    {
        auto const refused = filled(chosen, "cs", feature::opacity_micromap, fill::nvapi, route);
        REQUIRE(refused.has_error());
        CHECK(refused.error().contains("cs traces a ray query with flags that are no constant"))
            .dump("error", refused.error());
        // the DXIL route does not lower the library's runtime `has` of flags yet, whatever the fill
        if (route != native_route::text)
            continue;
        auto const native = filled(chosen, "cs", feature::opacity_micromap, fill::native, route);
        CHECK(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
        auto const without
            = sgl::compile_to_text({.source = chosen, .entry_point = "cs", .target = target::hlsl_dx12, .native = route});
        CHECK(without.has_value()).dump("error", without.has_error() ? without.error() : cc::string());
    }
}

TEST("sgl emit - the nvapi fill of opacity micromaps refuses a trace that forces 2 states, which NVAPI cannot "
     "(EMIT-184)")
{
    constexpr auto forced = "require ray_query\n"
                            "\n"
                            "binding work:\n"
                            "    world: acceleration_structure[.triangles]\n"
                            "    values: mut buffer[float]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                            "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n"
                            "    let flags = ray_flags.force_omm_2_state\n"
                            "    work.values[id.x] = work.world.trace(r, c => hit_decision.ignore, flags = flags).t\n";
    for (auto const route : k_routes)
    {
        auto const refused = filled(forced, "cs", feature::opacity_micromap, fill::nvapi, route);
        REQUIRE(refused.has_error());
        CHECK(refused.error().contains("cs forces a ray query's trace to 2 opacity micromap states, which the nvapi "
                                       "fill of opacity_micromap cannot"))
            .dump("error", refused.error());
        // natively it forces them, with the query's own flag
        auto const native = filled(forced, "cs", feature::opacity_micromap, fill::native, route);
        REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    }
    // on a device without micromaps there is nothing to force, and the flag is left out as before
    auto const without = sgl::compile_to_text({.source = forced, .entry_point = "cs", .target = target::hlsl_dx12});
    REQUIRE(without.has_value());
    CHECK(without.value().text.contains("& ~1024u)"));
    // and a query that forces nothing traces under the nvapi fill as it did
    auto const plain = filled(k_micromap_seen, "cs", feature::opacity_micromap, fill::nvapi, native_route::text);
    REQUIRE(plain.has_value()).dump("error", plain.has_error() ? plain.error() : cc::string());
}

TEST("sgl emit - the nvapi fill of reordering writes NVAPI's hit objects through nvHLSLExtns.h at 6.6 (EMIT-265)")
{
    auto const e = filled(k_reordering, "shoot", feature::shader_execution_reordering, fill::nvapi, native_route::text);
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const& text = e.value().text;
    // the extensions at the slot the host enables, in the text dx12 alone writes
    CHECK(text.contains("#define NV_SHADER_EXTN_SLOT u0\n#define NV_SHADER_EXTN_REGISTER_SPACE space13\n"
                        "#include \"nvHLSLExtns.h\"\n"));
    CHECK(text.contains("NvHitObject hit_object_1 = NvTraceRayHitObject(frame_world, "));
    CHECK(text.contains("NvReorderThread(hit_object_1, uint(uint(id.x)), uint(4));"));
    CHECK(text.contains("NvInvokeHitObject(frame_world, hit_object_1, p);"));
    CHECK(text.contains("NvHitObject h = NvTraceRayHitObject("));
    CHECK(text.contains("h.IsHit()"));
    CHECK(text.contains("h.IsMiss()"));
    CHECK(text.contains("h.GetRayDesc().TMax"));
    CHECK(text.contains("int(h.GetInstanceID())"));
    CHECK(text.contains("int(h.GetInstanceIndex())"));
    CHECK(text.contains("int(h.GetPrimitiveIndex())"));
    CHECK(text.contains("int(h.GetGeometryIndex())"));
    CHECK(text.contains("NvReorderThread(h)"));
    CHECK(!text.contains("dx::HitObject"));
    CHECK(!text.contains("MaybeReorderThread"));
    // NVAPI reorders at 6.6, where a payload's qualifiers need DXC's flag
    CHECK(e.value().dxil_shader_model == k_six_six);
    CHECK(e.value().needs_nvapi_header);
    REQUIRE(e.value().dxc_args.size() == 1);
    CHECK(e.value().dxc_args[0] == "-enable-payload-qualifiers");
    CHECK(e.value().features.has(feature::shader_execution_reordering));
    // the counter UAV is listed as a slot the host reserves, not one it binds
    auto reserved = 0;
    for (auto const& b : e.value().bindings)
        if (b.is_reserved)
        {
            ++reserved;
            CHECK(b.emitted == "g_NvidiaExt");
            CHECK(b.group == 13);
            CHECK(b.slot == 0);
            CHECK(b.access == "read_write");
            CHECK(b.stride == 256);
            CHECK(b.has_counter);
        }
    CHECK(reserved == 1);
    // its axis offers both fills
    REQUIRE(e.value().axes.size() == 1);
    CHECK(e.value().axes[0].f == feature::shader_execution_reordering);
    CHECK(same_fills(e.value().axes[0].fills, {fill::native, fill::nvapi}));
}

TEST("sgl emit - the native fill of reordering writes Shader Model 6.9's hit objects, and reserves no slot (EMIT-265)")
{
    auto const e = filled(k_reordering, "shoot", feature::shader_execution_reordering, fill::native, native_route::text);
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    CHECK(e.value().text.contains("dx::HitObject"));
    CHECK(!e.value().text.contains("nvHLSLExtns.h"));
    CHECK(!e.value().text.contains("NvHitObject"));
    CHECK(e.value().dxil_shader_model == dxil_shader_model{.minor = 9});
    CHECK(!e.value().needs_nvapi_header);
    CHECK(e.value().dxc_args.empty());
    for (auto const& b : e.value().bindings)
        CHECK(!b.is_reserved);
    // and a device without the feature traces plainly, with the same axis
    auto const plain
        = sgl::compile_to_text({.source = k_reordering, .entry_point = "shoot", .target = target::hlsl_dx12});
    REQUIRE(plain.has_value());
    CHECK(!plain.value().text.contains("HitObject"));
    CHECK(plain.value().dxil_shader_model == k_six_six);
    REQUIRE(plain.value().axes.size() == 1);
    CHECK(same_fills(plain.value().axes[0].fills, {fill::native, fill::nvapi}));
}

TEST("sgl emit - SGL's DXIL of the nvapi fill talks to the driver through the counter UAV at 6.6 (EMIT-265)")
{
    auto const e
        = filled(k_reordering, "shoot", feature::shader_execution_reordering, fill::nvapi, native_route::require);
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    CHECK(!e.value().dxil.empty());
    CHECK(container_model(e.value().dxil) == k_six_six);
    CHECK(e.value().dxil_shader_model == k_six_six);
    // SGL's DXIL includes no header, but the slot stands all the same
    CHECK(!e.value().needs_nvapi_header);
    auto reserved = 0;
    for (auto const& b : e.value().bindings)
        reserved += b.is_reserved ? 1 : 0;
    CHECK(reserved == 1);
}

namespace
{
/// A raygen of `body`, its lines at four spaces, over a pipeline with an array of structures beside its one structure.
[[nodiscard]] cc::string invoking(cc::string_view declarations, cc::string_view body)
{
    auto source = cc::string("require raytracing_pipeline\n"
                             "require binding_arrays\n"
                             "binding frame:\n"
                             "    world: acceleration_structure[.triangles]\n"
                             "    worlds: acceleration_structure[.triangles][4]\n"
                             "    seen: mut buffer[float]\n"
                             "    picks: buffer[int]\n"
                             "struct radiance:\n"
                             "    color: float3\n"
                             "rays path_rays:\n"
                             "    surface: radiance\n");
    source += declarations;
    source += "@raygen fun shoot(@launch_id id: int3){frame}:\n"
              "    require shader_execution_reordering\n"
              "    let mut p = radiance(float3(0.0, 0.0, 0.0))\n"
              "    let r = ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0))\n";
    auto is_line_start = true;
    for (auto const ch : body)
    {
        if (is_line_start)
            source += "    ";
        source += ch;
        is_line_start = ch == '\n';
    }
    source += "    frame.seen[id.x] = p.color.x\n"
              "@miss fun on_miss(p: mut radiance):\n"
              "    p.color = float3(1.0, 0.0, 0.0)\n"
              "@closest_hit fun on_hit(h: triangle_hit, p: mut radiance):\n"
              "    p.color = float3(0.0, 1.0, 0.0)\n"
              "hit_group hits for path_rays:\n"
              "    surface = (closest_hit = on_hit)\n"
              "@raytracing pipeline path:\n"
              "    rays = path_rays\n"
              "    raygen = shoot\n"
              "    miss.surface = on_miss\n"
              "    hit_groups = (hits)\n";
    return source;
}
} // namespace

TEST("sgl emit - the nvapi fill invokes a hit object through the structure its trace_hit took, wherever it is "
     "(EMIT-265)")
{
    struct invoked_case
    {
        cc::string_view what;
        cc::string_view declarations;
        cc::string_view body;
        cc::string_view structure;
    };
    invoked_case const cases[] = {
        {"its own let", "", "let h = trace_hit(frame.world, r, path_rays.surface, mut p)\nh.reorder()\nh.invoke(mut p)\n",
         "traced_structure"},
        {"a callee that traces and invokes",
         "fun run(q: mut radiance, s: ray){frame}:\n"
         "    let h = trace_hit(frame.world, s, path_rays.surface, mut q)\n"
         "    h.invoke(mut q)\n",
         "run(mut p, r)\n", "traced_structure"},
        {"a loop's body", "",
         "for i in 0 ..< 2:\n    let h = trace_hit(frame.world, r, path_rays.surface, mut p)\n    h.invoke(mut p)\n",
         "traced_structure"},
        {"an element by a let's index", "",
         "let k = id.x % 4\nlet h = trace_hit(frame.worlds[nonuniform k], r, path_rays.surface, mut p)\nh.invoke(mut "
         "p)\n",
         "traced_structure"},
        {"an element by a loop's index", "",
         "for i in 0 ..< 4:\n    let h = trace_hit(frame.worlds[i], r, path_rays.surface, mut p)\n    h.invoke(mut "
         "p)\n",
         "traced_structure"},
    };
    for (auto const& c : cases)
    {
        auto const source = invoking(c.declarations, c.body);
        for (auto const route : k_routes)
        {
            auto const e = filled(source, "shoot", feature::shader_execution_reordering, fill::nvapi, route);
            REQUIRE(e.has_value()).dump("case", c.what).dump("error", e.has_error() ? e.error() : cc::string());
            if (route == native_route::text)
                CHECK(e.value().text.contains(cc::format("NvInvokeHitObject({}", c.structure)))
                    .dump("case", c.what)
                    .dump("text", e.value().text);
            else
                CHECK(!e.value().dxil.empty()).dump("case", c.what);
        }
        // the native fill invokes it as Shader Model 6.9 does, through no structure
        auto const native
            = filled(source, "shoot", feature::shader_execution_reordering, fill::native, native_route::text);
        REQUIRE(native.has_value()).dump("case", c.what).dump("error", native.has_error() ? native.error() : cc::string());
        CHECK(native.value().text.contains("dx::HitObject::Invoke(")).dump("case", c.what);
    }
}

TEST("sgl emit - the nvapi fill pins the structure a trace_hit took, so a moving index moves nothing (EMIT-265)")
{
    struct pinned_case
    {
        cc::string_view what;
        cc::string_view declarations;
        cc::string_view body;
    };
    pinned_case const cases[] = {
        {"a while loop over a let mut index", "",
         "let mut k = 0\n"
         "while k < 4:\n"
         "    let h = trace_hit(frame.worlds[k], r, path_rays.surface, mut p)\n"
         "    h.invoke(mut p)\n"
         "    k = k + 1\n"},
        {"a let mut index never assigned again", "",
         "let mut k = id.x % 4\n"
         "let h = trace_hit(frame.worlds[nonuniform k], r, path_rays.surface, mut p)\n"
         "h.invoke(mut p)\n"},
        {"an index read from a buffer", "",
         "let h = trace_hit(frame.worlds[nonuniform frame.picks[id.x]], r, path_rays.surface, mut p)\n"
         "h.invoke(mut p)\n"},
        {"an index assigned between the trace and the invocation", "",
         "let mut k = id.x % 4\n"
         "let h = trace_hit(frame.worlds[nonuniform k], r, path_rays.surface, mut p)\n"
         "k = (k + 1) % 4\n"
         "h.invoke(mut p)\n"},
    };
    for (auto const& c : cases)
    {
        auto const source = invoking(c.declarations, c.body);
        for (auto const route : k_routes)
        {
            auto const e = filled(source, "shoot", feature::shader_execution_reordering, fill::nvapi, route);
            REQUIRE(e.has_value()).dump("case", c.what).dump("error", e.has_error() ? e.error() : cc::string());
            if (route == native_route::require)
            {
                CHECK(!e.value().dxil.empty()).dump("case", c.what);
                continue;
            }
            // the structure is copied once, at the trace, and both the trace and the invocation name the copy
            auto const& text = e.value().text;
            auto const at = text.find("RaytracingAccelerationStructure traced_structure");
            REQUIRE(at >= 0).dump("case", c.what).dump("text", text);
            auto const name_end = text.find(' ', at + 32);
            auto const pinned = cc::string(text.subview({.offset = at + 32, .size = name_end - at - 32}));
            CHECK(text.contains(cc::format("NvTraceRayHitObject({}, ", pinned))).dump("case", c.what).dump("text", text);
            CHECK(text.contains(cc::format("NvInvokeHitObject({}, ", pinned))).dump("case", c.what).dump("text", text);
        }
    }
}

namespace
{
/// The arguments of each call of `function` in `text`, split at their top-level commas.
[[nodiscard]] cc::vector<cc::vector<cc::string>> calls_of(cc::string_view text, cc::string_view function)
{
    auto result = cc::vector<cc::vector<cc::string>>();
    auto const opening = cc::format("{}(", function);
    for (auto at = text.find(opening); at >= 0; at = text.find(opening, at + 1))
    {
        auto arguments = cc::vector<cc::string>();
        arguments.emplace_back();
        auto depth = 0;
        for (auto i = at + opening.size(); i < text.size(); ++i)
        {
            auto const ch = text[i];
            if (depth == 0 && ch == ')')
                break;
            if (ch == '(' || ch == '[')
                ++depth;
            if (ch == ')' || ch == ']')
                --depth;
            if (depth == 0 && ch == ',')
            {
                arguments.emplace_back();
                continue;
            }
            if (!(ch == ' ' && arguments.back().empty()))
                arguments.back() += ch;
        }
        result.push_back(cc::move(arguments));
    }
    return result;
}

/// The hit object `NvTraceRayHitObject` gave on the line it is called at in `text`, in order of the calls.
[[nodiscard]] cc::vector<cc::string> traced_hit_objects(cc::string_view text)
{
    auto result = cc::vector<cc::string>();
    auto const marker = cc::string_view("const NvHitObject ");
    for (auto at = text.find(marker); at >= 0; at = text.find(marker, at + 1))
    {
        auto const start = at + marker.size();
        auto const end = text.find(' ', start);
        result.push_back(cc::string(text.subview({.offset = start, .size = end - start})));
    }
    return result;
}
} // namespace

TEST("sgl emit - the nvapi fill invokes each hit object through the very structure its trace took, where values move "
     "(EMIT-265)")
{
    // a hit object is invoked through what its own trace was handed, however the source reaches that structure
    struct invoked_case
    {
        cc::string_view what;
        cc::string_view declarations;
        cc::string_view body;
    };
    invoked_case const cases[] = {
        {"a callee's mut parameter assigning the variable its structure's index came from",
         "fun go(s: acceleration_structure[.triangles], i: mut int, q: mut radiance, w: ray):\n"
         "    let h = trace_hit(s, w, path_rays.surface, mut q)\n"
         "    i = i + 1\n"
         "    h.invoke(mut q)\n",
         "let mut k = id.x % 4\ngo(frame.worlds[nonuniform k], mut k, mut p, r)\n"},
        {"two hit objects of two structures, one after the other", "",
         "let h = trace_hit(frame.world, r, path_rays.surface, mut p)\nh.invoke(mut p)\n"
         "let g = trace_hit(frame.worlds[1], r, path_rays.surface, mut p)\ng.invoke(mut p)\n"},
        {"one callee inlined twice, each with its own structure",
         "fun run(q: mut radiance, s: ray, j: int){frame}:\n"
         "    let h = trace_hit(frame.worlds[j], s, path_rays.surface, mut q)\n"
         "    h.invoke(mut q)\n",
         "run(mut p, r, 0)\nrun(mut p, r, 1)\n"},
        {"two hit objects alive at once, invoked in the other order", "",
         "let h = trace_hit(frame.world, r, path_rays.surface, mut p)\n"
         "let g = trace_hit(frame.worlds[2], r, path_rays.surface, mut p)\ng.invoke(mut p)\nh.invoke(mut p)\n"},
    };
    for (auto const& c : cases)
    {
        auto const source = invoking(c.declarations, c.body);
        auto const e = filled(source, "shoot", feature::shader_execution_reordering, fill::nvapi, native_route::text);
        REQUIRE(e.has_value()).dump("case", c.what).dump("error", e.has_error() ? e.error() : cc::string());
        auto const& text = e.value().text;
        auto const traces = calls_of(text, "NvTraceRayHitObject");
        auto const hits = traced_hit_objects(text);
        auto const invokes = calls_of(text, "NvInvokeHitObject");
        REQUIRE(traces.size() == hits.size()).dump("case", c.what).dump("text", text);
        REQUIRE(!invokes.empty()).dump("case", c.what).dump("text", text);
        for (auto const& invoke : invokes)
        {
            REQUIRE(invoke.size() == 3).dump("case", c.what).dump("text", text);
            auto found = false;
            for (auto i = isize(0); i < hits.size(); ++i)
                if (hits[i] == invoke[1])
                {
                    found = true;
                    CHECK(invoke[0] == traces[i][0]).dump("case", c.what).dump("hit object", invoke[1]).dump("text", text);
                }
            CHECK(found).dump("case", c.what).dump("hit object", invoke[1]).dump("text", text);
        }
        // SGL's DXIL takes it too
        auto const native
            = filled(source, "shoot", feature::shader_execution_reordering, fill::nvapi, native_route::require);
        REQUIRE(native.has_value()).dump("case", c.what).dump("error", native.has_error() ? native.error() : cc::string());
        CHECK(!native.value().dxil.empty()).dump("case", c.what);
    }
}

TEST("sgl emit - the nvapi fill of opacity micromaps writes nothing, at 6.6 (EMIT-265, EMIT-184)")
{
    for (auto const route : k_routes)
    {
        auto const nvapi = filled(k_micromap_seen, "cs", feature::opacity_micromap, fill::nvapi, route);
        REQUIRE(nvapi.has_value()).dump("error", nvapi.has_error() ? nvapi.error() : cc::string());
        CHECK(nvapi.value().dxil_shader_model == k_six_six);
        CHECK(nvapi.value().features.has(feature::opacity_micromap));
        CHECK(!nvapi.value().needs_nvapi_header);
        auto const native = filled(k_micromap_seen, "cs", feature::opacity_micromap, fill::native, route);
        REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
        CHECK(native.value().dxil_shader_model == dxil_shader_model{.minor = 9});
        if (route == native_route::text)
        {
            CHECK(!nvapi.value().text.contains("OPACITY_MICROMAPS"));
            CHECK(native.value().text.contains(", RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>"));
        }
        else
        {
            CHECK(container_model(nvapi.value().dxil) == k_six_six);
            CHECK(container_model(native.value().dxil) == dxil_shader_model{.minor = 9});
        }
    }
}
