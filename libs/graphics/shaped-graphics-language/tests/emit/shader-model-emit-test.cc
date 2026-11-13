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

// What a dx12 output needs of the shader model (EMIT-262), by the fill a compile asks for each feature and core
// construct (EMIT-263), and the variants an entry point's consults make (EMIT-264).

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
} // namespace

TEST("sgl emit - each feature and core construct names the DXIL shader model its fill needs (EMIT-262)")
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

TEST("sgl emit - each target offers its fills, most demanding first (EMIT-263)")
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

TEST("sgl emit - the native fill of the draw's start reads it as system values and states 6.8, the default (EMIT-262)")
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

TEST("sgl emit - a dx12 entry that reads no draw start stays at 6.6 under either core fill (EMIT-262)")
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

TEST("sgl emit - a dx12 geometry or tessellation stage stays at 6.6 under the 6.6 fills (EMIT-262)")
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

TEST("sgl emit - an entry's axes are the features it consults, granted or not, with each target's fills (EMIT-264)")
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

TEST("sgl emit - a variant's key names its granted features with their fills, then the core fills that are not native")
{
    CHECK(sgl::variant_key().to_string() == "");
    auto const key = sgl::variant_key{.fills = {{.f = feature::opacity_micromap, .how = fill::nvapi},
                                                {.f = feature::shader_execution_reordering, .how = fill::native}},
                                      .core = k_six_six_fills};
    CHECK(key.to_string()
          == "shader_execution_reordering=native;opacity_micromap=nvapi;draw_start=root_constant;wave_size=none");
    CHECK(key.granted() == (feature::shader_execution_reordering | feature::opacity_micromap));
    CHECK(sgl::variant_key{.core = {.wave_size = fill::none}}.to_string() == "wave_size=none");
}

TEST("sgl emit - the nvapi fill of reordering writes NVAPI's hit objects through nvHLSLExtns.h at 6.6 (EMIT-263)")
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
        }
    CHECK(reserved == 1);
    // its axis offers both fills
    REQUIRE(e.value().axes.size() == 1);
    CHECK(e.value().axes[0].f == feature::shader_execution_reordering);
    CHECK(same_fills(e.value().axes[0].fills, {fill::native, fill::nvapi}));
}

TEST("sgl emit - the native fill of reordering writes Shader Model 6.9's hit objects, and reserves no slot (EMIT-263)")
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

TEST("sgl emit - SGL's DXIL of the nvapi fill talks to the driver through the counter UAV at 6.6 (EMIT-263)")
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

TEST("sgl emit - the nvapi fill of reordering refuses a hit object invoked apart from its trace (EMIT-263)")
{
    auto source = cc::string(k_reordering);
    auto const at = source.find("        h.reorder()\n");
    REQUIRE(at >= 0);
    source = cc::format("{}        h.invoke(mut p)\n{}", source.subview({.offset = 0, .size = at}),
                        source.subview({.offset = at, .size = source.size() - at}));
    for (auto const route : k_routes)
    {
        auto const refused = filled(source, "shoot", feature::shader_execution_reordering, fill::nvapi, route);
        REQUIRE(refused.has_error());
        CHECK(refused.error().contains("shoot invokes a hit object apart from its trace, which the nvapi fill of "
                                       "shader_execution_reordering cannot"));
    }
    // the native fill invokes it
    auto const native = filled(source, "shoot", feature::shader_execution_reordering, fill::native, native_route::text);
    REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
    CHECK(native.value().text.contains("dx::HitObject::Invoke(h, "));
}

TEST("sgl emit - the nvapi fill of opacity micromaps writes nothing, at 6.6 (EMIT-263, EMIT-184)")
{
    for (auto const route : k_routes)
    {
        auto const nvapi = filled(k_micromap_query, "cs", feature::opacity_micromap, fill::nvapi, route);
        REQUIRE(nvapi.has_value()).dump("error", nvapi.has_error() ? nvapi.error() : cc::string());
        CHECK(nvapi.value().dxil_shader_model == k_six_six);
        CHECK(nvapi.value().features.has(feature::opacity_micromap));
        CHECK(!nvapi.value().needs_nvapi_header);
        auto const native = filled(k_micromap_query, "cs", feature::opacity_micromap, fill::native, route);
        REQUIRE(native.has_value()).dump("error", native.has_error() ? native.error() : cc::string());
        CHECK(native.value().dxil_shader_model == dxil_shader_model{.minor = 9});
        if (route == native_route::text)
        {
            CHECK(!nvapi.value().text.contains("OPACITY_MICROMAPS"));
            CHECK(!nvapi.value().text.contains("FORCE_OMM_2_STATE"));
            CHECK(native.value().text.contains(", RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>"));
        }
        else
        {
            CHECK(container_model(nvapi.value().dxil) == k_six_six);
            CHECK(container_model(native.value().dxil) == dxil_shader_model{.minor = 9});
        }
    }
}
