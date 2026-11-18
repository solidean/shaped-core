#include "../emit/emit-test-support.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/driver/describe.hh>

using namespace sgl_test;
using sgl::check::stage;
using sgl::emit::target;

// What one entry point's signature hands its pipeline beside its bindings: its vertex input, its payload and its hit
// attributes (EMIT-268, EMIT-269, EMIT-270). A host that builds its pipelines in its own code reads them per entry point,
// from `describe` or from `compile_to_text`, and the two say the same.

namespace
{
/// An entry point of every stage that has an interface to state, and of the stages that state none.
constexpr auto k_every_stage = cc::string_view(R"(require raytracing_pipeline

@vertex struct mesh_vertex:
    position: pos3
    color: float3

struct varyings:
    @position position: hpos4
    color: float3

@pixel struct shaded:
    color: float4

@vertex fun fed(v: mesh_vertex, @instance_index i: int) -> varyings:
    return { position = hpos4(v.position.x, v.position.y, v.position.z, 1.0), color = v.color }

@vertex fun pulled(@vertex_index i: int) -> varyings:
    return { position = hpos4(0.0, 0.0, 0.0, 1.0), color = float3(1.0, 1.0, 1.0) }

@pixel fun shade_ps(p: varyings) -> shaded:
    return { color = float4(..p.color, 1.0) }

@compute(8) fun clear(@thread_id id: int3):
    let x = id.x

enum tag:
    plain
    glossy

// a float3 and an enum: 16 bytes
struct radiance:
    color: float3
    kind: tag

// two floats and an enum: 12 bytes, which no triangle hit is
struct sphere_attributes:
    u: float
    v: float
    kind: tag

struct operand:
    x: float
    y: float

rays rs:
    primary: radiance

@raygen fun start(@launch_id id: int3):
    let x = id.x

@miss fun sky(p: mut radiance):
    p.color = float3(1.0, 1.0, 1.0)

@closest_hit fun shade_triangle(h: triangle_hit, p: mut radiance):
    p.color = float3(h.barycentrics.x, h.barycentrics.y, 0.0)

@any_hit fun cut_triangle(c: triangle_candidate, p: mut radiance) -> hit_decision:
    return hit_decision.accept

@closest_hit fun shade_sphere(h: procedural_hit[sphere_attributes], p: mut radiance):
    p.color = float3(h.attributes.u, h.attributes.v, 0.0)

@any_hit fun cut_sphere(c: procedural_candidate[sphere_attributes], p: mut radiance) -> hit_decision:
    return hit_decision.accept

@intersection fun sphere(b: procedural_box) -> report[sphere_attributes]:
    return report.none()

@callable fun doubled(v: mut operand):
    v.x = v.x * 2.0
)");

/// What one entry point states, as both reports give it.
struct expected_interface
{
    cc::string_view name;
    stage entry_stage = stage::none;
    cc::string_view vertex_input;
    cc::string_view payload;
    i32 payload_size = 0;
    cc::string_view attributes;
    i32 attribute_size = 0;
};

constexpr expected_interface k_expected[] = {
    {.name = "fed", .entry_stage = stage::vertex, .vertex_input = "mesh_vertex"},
    // CHK-88: a vertex entry point that takes no stage struct draws from no vertex buffer
    {.name = "pulled", .entry_stage = stage::vertex},
    {.name = "shade_ps", .entry_stage = stage::pixel},
    {.name = "clear", .entry_stage = stage::compute},
    {.name = "start", .entry_stage = stage::raygen},
    {.name = "sky", .entry_stage = stage::miss, .payload = "radiance", .payload_size = 16},
    {.name = "shade_triangle",
     .entry_stage = stage::closest_hit,
     .payload = "radiance",
     .payload_size = 16,
     .attribute_size = 8},
    {.name = "cut_triangle", .entry_stage = stage::any_hit, .payload = "radiance", .payload_size = 16, .attribute_size = 8},
    {.name = "shade_sphere",
     .entry_stage = stage::closest_hit,
     .payload = "radiance",
     .payload_size = 16,
     .attributes = "sphere_attributes",
     .attribute_size = 12},
    {.name = "cut_sphere",
     .entry_stage = stage::any_hit,
     .payload = "radiance",
     .payload_size = 16,
     .attributes = "sphere_attributes",
     .attribute_size = 12},
    {.name = "sphere", .entry_stage = stage::intersection, .attributes = "sphere_attributes", .attribute_size = 12},
    {.name = "doubled", .entry_stage = stage::callable, .payload = "operand", .payload_size = 8},
};

sgl::module_description described(cc::string_view source)
{
    auto const r = sgl::describe({.source = source, .source_name = "t.sgl"});
    if (r.has_error())
        FAIL(r.error());
    return r.value();
}

sgl::described_entry_point const& entry_named(sgl::module_description const& d, cc::string_view name)
{
    for (auto const& e : d.entry_points)
        if (e.name == name)
            return e;
    FAIL(cc::format("no entry point {} was described", name));
    return d.entry_points[0];
}

sgl::emitted_source compiled(cc::string_view source, cc::string_view entry_point, target t = target::hlsl_dx12)
{
    auto const r
        = sgl::compile_to_text({.source = source, .source_name = "t.sgl", .entry_point = entry_point, .target = t});
    if (r.has_error())
        FAIL(r.error());
    return r.value();
}

/// The structural hash of the struct `name` as the source's own entry points name it, empty for none.
cc::string shape_of(sgl::module_description const& d, cc::string_view name)
{
    for (auto const& e : d.entry_points)
        if (e.payload == name)
            return e.payload_shape;
    return {};
}
} // namespace

TEST("sgl entry interface - describe states each stage's vertex input, payload and hit attributes")
{
    auto const d = described(k_every_stage);
    for (auto const& x : k_expected)
        SECTION("{}", x.name)
        {
            auto const& e = entry_named(d, x.name);
            CHECK(e.stage == x.entry_stage);
            CHECK(e.vertex_input == x.vertex_input);
            CHECK(e.payload == x.payload);
            CHECK(e.payload_size == x.payload_size);
            CHECK(e.payload_shape.empty() == x.payload.empty());
            CHECK(e.attributes == x.attributes);
            CHECK(e.attribute_size == x.attribute_size);
            CHECK(e.attributes_shape.empty() == x.attributes.empty());
        }
    // the vertex input it names is one `structs` describes, with what an input layout needs of each member
    auto const& structs = d.structs;
    REQUIRE(!structs.empty());
    CHECK(structs[0].name == "mesh_vertex");
    CHECK(structs[0].edge == stage::vertex);
    REQUIRE(structs[0].members.size() == 2);
    CHECK(structs[0].members[1].semantic == "COLOR");
}

TEST("sgl entry interface - the payload's size is its ray set's, and the attributes' are the pipeline's")
{
    auto const d = described(cc::string(k_every_stage)
                             + "hit_group round for rs:\n"
                               "    geometry = .procedural\n"
                               "    intersection = sphere\n"
                               "    primary = (closest_hit = shade_sphere, any_hit = cut_sphere)\n"
                               "@raytracing pipeline path:\n"
                               "    rays = rs\n"
                               "    raygen = start\n"
                               "    hit_groups = (round)\n");
    REQUIRE(d.ray_sets.size() == 1);
    REQUIRE(d.ray_sets[0].payload_sizes.size() == 1);
    CHECK(entry_named(d, "sky").payload_size == d.ray_sets[0].payload_sizes[0]);
    CHECK(entry_named(d, "sky").payload_shape == d.ray_sets[0].payload_shapes[0]);
    REQUIRE(d.raytracing_pipelines.size() == 1);
    CHECK(entry_named(d, "sphere").attribute_size == d.raytracing_pipelines[0].max_attribute_size);
    CHECK(entry_named(d, "shade_sphere").attributes_shape == entry_named(d, "sphere").attributes_shape);
}

TEST("sgl entry interface - a struct's shape is the same whether it is a payload or hit attributes")
{
    // the same members, their names included, under another struct name: the attributes' shape is the structural hash
    // a payload's is
    auto const source = cc::string(k_every_stage)
                      + "struct same_as_attributes:\n"
                        "    u: float\n"
                        "    v: float\n"
                        "    kind: tag\n"
                        "@callable fun carried(a: mut same_as_attributes):\n"
                        "    a.u = 1.0\n";
    auto const d = described(source);
    CHECK(shape_of(d, "same_as_attributes") == entry_named(d, "sphere").attributes_shape);
    CHECK(entry_named(d, "carried").payload_size == entry_named(d, "sphere").attribute_size);
}

TEST("sgl entry interface - compile_to_text states what describe states, on every target that has the stage")
{
    auto const d = described(k_every_stage);
    for (auto const& x : k_expected)
        SECTION("{}", x.name)
        {
            auto const& described_entry = entry_named(d, x.name);
            // a ray-tracing stage is compiled for dx12 and vulkan alone: WGSL has no pipeline of them, and metal fuses
            // its hit stages into a traversal of its own
            auto const is_raytracing = x.entry_stage >= stage::raygen;
            for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl, target::wgsl})
            {
                if (is_raytracing && (t == target::msl || t == target::wgsl))
                    continue;
                auto const e = compiled(k_every_stage, x.name, t);
                CHECK(e.stage == x.entry_stage);
                CHECK(e.vertex_input == described_entry.vertex_input);
                CHECK(e.payload == described_entry.payload);
                CHECK(e.payload_shape == described_entry.payload_shape);
                CHECK(e.payload_size == described_entry.payload_size);
                CHECK(e.attributes == described_entry.attributes);
                CHECK(e.attributes_shape == described_entry.attributes_shape);
                CHECK(e.attribute_size == described_entry.attribute_size);
            }
        }
}

TEST("sgl entry interface - a module's vertex input is named by the module's name, not the program's alias for it")
{
    sgl::library_file const library[] = {{.name = "view.sgl",
                                          .source = "module view\n"
                                                    "@vertex struct quad:\n"
                                                    "    position: pos3\n"}};
    auto const source
        = cc::string_view("use view as v\n"
                          "struct varyings:\n"
                          "    @position position: hpos4\n"
                          "@vertex fun from_module(q: v.quad) -> varyings:\n"
                          "    return { position = hpos4(q.position.x, q.position.y, q.position.z, 1.0) }\n");
    auto const d = sgl::describe({.source = source, .source_name = "t.sgl", .library = library});
    REQUIRE(d.has_value());
    REQUIRE(d.value().entry_points.size() == 1);
    CHECK(d.value().entry_points[0].vertex_input == "view.quad");
    auto const e = sgl::compile_to_text({.source = source,
                                         .source_name = "t.sgl",
                                         .library = library,
                                         .entry_point = "from_module",
                                         .target = target::hlsl_dx12});
    REQUIRE(e.has_value());
    CHECK(e.value().vertex_input == "view.quad");
}

TEST("sgl entry interface - nested, generic and a module's structs are sized by every member, alike on both reports")
{
    // a word per scalar, an enum and a bool each one word, summed through nested and generic structs, with no padding
    sgl::library_file const library[] = {{.name = "view.sgl",
                                          .source = "module view\n"
                                                    "struct ray_load:\n"
                                                    "    color: float3\n"
                                                    "    hits: int\n"
                                                    "struct sphere_attributes:\n"
                                                    "    u: float\n"
                                                    "    v: float\n"
                                                    "    w: float\n"}};
    auto const source = cc::string_view(R"(require raytracing_pipeline
use view

struct inner:
    a: float3
    b: int

struct big:
    first: inner
    second: inner
    m: float2
    flag: bool

struct nested_attributes:
    pair: inner
    w: float

struct pair[T]:
    a: T
    b: T

@miss fun sky(p: mut big):
    p.m = float2(1.0, 1.0)

@closest_hit fun shade(h: procedural_hit[nested_attributes], p: mut big):
    p.m = float2(h.attributes.w, 0.0)

@intersection fun isect(b: procedural_box) -> report[nested_attributes]:
    return report.none()

@any_hit fun cut(c: procedural_candidate[pair[int]], p: mut pair[float]) -> hit_decision:
    return hit_decision.accept

@miss fun module_sky(p: mut view.ray_load):
    p.hits = 0

@closest_hit fun module_shade(h: procedural_hit[view.sphere_attributes], p: mut view.ray_load):
    p.hits = 1
)");
    auto const r = sgl::describe({.source = source, .source_name = "t.sgl", .library = library});
    if (r.has_error())
        FAIL(r.error());
    auto const& d = r.value();
    CHECK(entry_named(d, "sky").payload_size == 44);
    CHECK(entry_named(d, "shade").payload_size == 44);
    CHECK(entry_named(d, "shade").attributes == "nested_attributes");
    CHECK(entry_named(d, "shade").attribute_size == 20);
    CHECK(entry_named(d, "isect").attribute_size == 20);
    CHECK(entry_named(d, "isect").attributes_shape == entry_named(d, "shade").attributes_shape);
    // an instance of a generic struct by its host name, as the target text names it (EMIT-179)
    CHECK(entry_named(d, "cut").payload == "pair_float");
    CHECK(entry_named(d, "cut").payload_size == 8);
    CHECK(entry_named(d, "cut").attributes == "pair_int");
    CHECK(entry_named(d, "cut").attribute_size == 8);
    CHECK(entry_named(d, "module_sky").payload_size == 16);
    CHECK(entry_named(d, "module_shade").attribute_size == 12);
    CHECK(entry_named(d, "module_shade").attributes_shape.size() == 32);
    for (auto const name : {"sky", "shade", "isect", "cut", "module_sky", "module_shade"})
        SECTION("{}", name)
        {
            auto const& described_entry = entry_named(d, name);
            for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
            {
                auto const c = sgl::compile_to_text(
                    {.source = source, .source_name = "t.sgl", .library = library, .entry_point = name, .target = t});
                if (c.has_error())
                    FAIL(c.error());
                auto const& e = c.value();
                CHECK(e.payload == described_entry.payload);
                CHECK(e.payload_shape == described_entry.payload_shape);
                CHECK(e.payload_size == described_entry.payload_size);
                CHECK(e.attributes == described_entry.attributes);
                CHECK(e.attributes_shape == described_entry.attributes_shape);
                CHECK(e.attribute_size == described_entry.attribute_size);
            }
        }
}

namespace
{
/// A module declaring a struct of each bare name the program declares too, and a ray set over its own payload.
constexpr cc::string_view k_twin_module = R"(module view

struct radiance:
    color: float3

struct hit_uv:
    u: float
    v: float
    w: float

struct operand:
    x: float
    y: float

struct particle:
    q: float4

struct glow:
    q: float4

@vertex struct mesh_vertex:
    position: pos3
    weight: float

rays shared:
    primary: radiance
)";

/// The program's own struct of each of those names, reached beside the module's, which the program uses by an alias.
constexpr cc::string_view k_twin_program = R"(require raytracing_pipeline
use view as v

struct radiance:
    color: float4

struct hit_uv:
    u: float
    v: float

struct operand:
    x: float

struct particle:
    p: float

@vertex struct mesh_vertex:
    position: pos3

rays own_rays:
    primary: radiance

binding frame:
    world: acceleration_structure[.triangles]
    theirs: buffer[v.particle]
    own: buffer[particle]
    nearest: v.glow[2]

struct varyings:
    @position position: hpos4

@pixel struct shaded:
    color: float4

@vertex fun own_vs(m: mesh_vertex) -> varyings:
    return { position = hpos4(m.position.x, m.position.y, m.position.z, 1.0) }

@vertex fun module_vs(m: v.mesh_vertex) -> varyings:
    return { position = hpos4(m.position.x, m.position.y, m.position.z, m.weight) }

@pixel fun shade_ps(p: varyings) -> shaded:
    return { color = float4(1.0, 1.0, 1.0, 1.0) }

@raygen fun start(@launch_id id: int3){frame}:
    let mut q = v.radiance(float3(frame.theirs[0].q.x + frame.own[0].p + frame.nearest[1].q.y, 0.0, 0.0))
    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), v.shared.primary, mut q)

@miss fun own_sky(p: mut radiance):
    p.color = float4(1.0, 1.0, 1.0, 1.0)

@miss fun module_sky(p: mut v.radiance):
    p.color = float3(1.0, 1.0, 1.0)

@closest_hit fun own_hit(h: procedural_hit[hit_uv], p: mut radiance):
    p.color = float4(h.attributes.u, h.attributes.v, 0.0, 1.0)

@closest_hit fun module_hit(h: procedural_hit[v.hit_uv], p: mut v.radiance):
    p.color = float3(h.attributes.u, h.attributes.v, h.attributes.w)

@intersection fun module_isect(b: procedural_box) -> report[v.hit_uv]:
    return report.none()

@callable fun own_call(o: mut operand):
    o.x = o.x * 2.0

@callable fun module_call(o: mut v.operand):
    o.y = o.x

callables own_ops = (own_call)

callables module_ops = (module_call, .host)

hit_group round for v.shared:
    geometry = .procedural
    intersection = module_isect
    primary = (closest_hit = module_hit)

@raytracing pipeline path:
    rays = v.shared
    raygen = start
    hit_groups = (round)

pipeline drawn:
    vertex = module_vs
    pixel = shade_ps
    format = .bgra8_unorm
)";

/// The ray set the description names `name`; fails the test where it names none.
sgl::described_ray_set const& ray_set_named(sgl::module_description const& d, cc::string_view name)
{
    for (auto const& s : d.ray_sets)
        if (s.name == name)
            return s;
    FAIL(cc::format("no ray set {} was described", name));
    return d.ray_sets[0];
}

/// Whether one of `lines` starts with `prefix`.
bool any_starts_with(cc::span<cc::string const> lines, cc::string_view prefix)
{
    for (auto const& line : lines)
        if (line.starts_with(prefix))
            return true;
    return false;
}
} // namespace

TEST("sgl entry interface - every struct the description names is its host name, a module's apart from the file's own")
{
    // a struct of the module and the program's own of one bare name are told apart by `m.name`, the module's name and
    // not the program's alias for it, wherever the description names a struct
    sgl::library_file const library[] = {{.name = "view.sgl", .source = k_twin_module}};
    auto const r = sgl::describe({.source = k_twin_program, .source_name = "t.sgl", .library = library});
    if (r.has_error())
        FAIL(r.error());
    auto const& d = r.value();

    struct named
    {
        cc::string_view entry;
        cc::string_view vertex_input;
        cc::string_view payload;
        cc::string_view attributes;
    };
    constexpr named k_names[] = {
        {.entry = "own_vs", .vertex_input = "mesh_vertex"},
        {.entry = "module_vs", .vertex_input = "view.mesh_vertex"},
        {.entry = "own_sky", .payload = "radiance"},
        {.entry = "module_sky", .payload = "view.radiance"},
        {.entry = "own_hit", .payload = "radiance", .attributes = "hit_uv"},
        {.entry = "module_hit", .payload = "view.radiance", .attributes = "view.hit_uv"},
        {.entry = "module_isect", .attributes = "view.hit_uv"},
        {.entry = "own_call", .payload = "operand"},
        {.entry = "module_call", .payload = "view.operand"},
    };
    for (auto const& x : k_names)
        SECTION("{}", x.entry)
        {
            auto const& e = entry_named(d, x.entry);
            CHECK(e.vertex_input == x.vertex_input);
            CHECK(e.payload == x.payload);
            CHECK(e.attributes == x.attributes);
            // compile_to_text names each as describe does, on both targets that have every stage
            for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
            {
                auto const c = sgl::compile_to_text({.source = k_twin_program,
                                                     .source_name = "t.sgl",
                                                     .library = library,
                                                     .entry_point = x.entry,
                                                     .target = t});
                if (c.has_error())
                    FAIL(c.error());
                CHECK(c.value().vertex_input == x.vertex_input);
                CHECK(c.value().payload == x.payload);
                CHECK(c.value().attributes == x.attributes);
            }
        }
    // the two payloads differ in shape as well as in name
    CHECK(entry_named(d, "own_sky").payload_shape != entry_named(d, "module_sky").payload_shape);

    // a ray set's payloads, the program's own set's and the module's
    REQUIRE(ray_set_named(d, "own_rays").payloads.size() == 1);
    CHECK(ray_set_named(d, "own_rays").payloads[0] == "radiance");
    REQUIRE(ray_set_named(d, "view.shared").payloads.size() == 1);
    CHECK(ray_set_named(d, "view.shared").payloads[0] == "view.radiance");

    // a callables table's parameter, and the one a host's callable takes
    REQUIRE(d.callables.size() == 2);
    CHECK(d.callables[0].parameter == "operand");
    CHECK(d.callables[1].parameter == "view.operand");
    REQUIRE(d.raytracing_pipelines.size() == 1);
    auto const& rt = d.raytracing_pipelines[0];
    CHECK(rt.host_callable_parameter == "view.operand");
    CHECK(rt.host_callable_shape == d.callables[1].parameter_shape);
    // and the frozen lines, which a reload compares, name them as the fields do
    CHECK(any_starts_with(rt.frozen, "rays = view.shared: primary view.radiance@"));
    CHECK(any_starts_with(
        rt.frozen, cc::format("callables = own_call, module_call, .host view.operand@{}", rt.host_callable_shape)));

    // a raster pipeline's vertex input, in its field and its frozen line
    REQUIRE(d.pipelines.size() == 1);
    CHECK(d.pipelines[0].vertex_input == "view.mesh_vertex");
    CHECK(any_starts_with(d.pipelines[0].frozen, "vertex input = view.mesh_vertex@"));

    // the structs the binding places in memory, and its members' types, an array of a module's struct by its element
    auto memory_names = cc::vector<cc::string>();
    for (auto const& s : d.memory_structs)
        memory_names.push_back(s.name);
    CHECK(memory_names.size() == 3);
    CHECK(memory_names.values().any([](cc::string const& n) { return n == "view.glow"; }));
    CHECK(memory_names.values().any([](cc::string const& n) { return n == "view.particle"; }));
    CHECK(memory_names.values().any([](cc::string const& n) { return n == "particle"; }));
    REQUIRE(d.bindings.size() == 1);
    auto const& members = d.bindings[0].members;
    REQUIRE(members.size() == 4);
    CHECK(members[1].type == "view.particle");
    CHECK(members[2].type == "particle");
    CHECK(members[3].type == "view.glow[2]");
}

namespace
{
/// A vertex input of every kind of member a layout distinguishes: a plain one, a `@format`, a `@stream`, an array, a
/// matrix and a `@per_instance` one, in one stream of its own.
constexpr auto k_mesh_vertex = cc::string_view("@vertex struct mesh_vertex:\n"
                                               "    position: pos3\n"
                                               "    @format(.rgba8_unorm) tint: float4\n"
                                               "    @stream(normals) normal: vec3\n"
                                               "    uv: float2[2]\n"
                                               "    @per_instance frame: mat4\n"
                                               "    @per_instance @stream(instances) scale: float\n");

/// A vertex entry point that reads every member of `mesh_vertex`, named as `name` reaches it.
[[nodiscard]] cc::string vertex_entry_of(cc::string_view name)
{
    return cc::format("struct varyings:\n"
                      "    @position position: hpos4\n"
                      "@vertex fun fed(v: {}) -> varyings:\n"
                      "    return {{ position = v.frame * hpos4(v.position.x * v.scale + v.uv[1].x + v.tint.x + "
                      "v.normal.x, v.position.y, v.position.z, 1.0) }}\n",
                      name);
}

/// `mesh_vertex`'s attributes as `vertex_attributes` states them: one line each, a piece of `uv` and of `frame` each.
constexpr auto k_mesh_attributes = cc::string_view("position pos3 POSITION0 @0 per_vertex\n"
                                                   "tint float4 TINT0 @1 rgba8_unorm per_vertex\n"
                                                   "normal vec3 NORMAL0 @2 normals\n"
                                                   "uv float2 UV0 @3 per_vertex\n"
                                                   "uv float2 UV1 @4 per_vertex\n"
                                                   "frame float4 FRAME0 @5 per_instance per_instance\n"
                                                   "frame float4 FRAME1 @6 per_instance per_instance\n"
                                                   "frame float4 FRAME2 @7 per_instance per_instance\n"
                                                   "frame float4 FRAME3 @8 per_instance per_instance\n"
                                                   "scale float SCALE0 @9 instances per_instance\n");

/// `attributes` one line each: name, type, semantic and its index, location, format where stated, stream, and the rate.
[[nodiscard]] cc::string layout_text(cc::span<sgl::described_vertex_attribute const> attributes)
{
    auto text = cc::string();
    for (auto const& a : attributes)
        text += cc::format("{} {} {}{} @{}{}{} {}{}\n", a.name, a.type, a.semantic, a.semantic_index, a.location,
                           a.format.empty() ? "" : " ", a.format, a.stream, a.is_per_instance ? " per_instance" : "");
    return text;
}

/// That `fed` of `source` states `vertex_input` and `k_mesh_attributes` in describe and on every target, wherever
/// `mesh_vertex` is declared, and that `describe` describes no struct the source does not declare.
void check_mesh_layout(cc::string_view source,
                       cc::span<sgl::library_file const> library,
                       cc::string_view vertex_input,
                       bool is_own)
{
    auto const d = sgl::describe({.source = source, .source_name = "t.sgl", .library = library});
    if (d.has_error())
        FAIL(d.error());
    auto const& e = entry_named(d.value(), "fed");
    CHECK(e.vertex_input == vertex_input);
    CHECK(layout_text(e.vertex_attributes) == k_mesh_attributes);
    // the struct is declared once: the description of the file holding it is where its C++ type comes from
    CHECK(d.value().structs.size() == (is_own ? 1 : 0));
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::wgsl, target::msl})
    {
        auto const c = sgl::compile_to_text(
            {.source = source, .source_name = "t.sgl", .library = library, .entry_point = "fed", .target = t});
        if (c.has_error())
            FAIL(c.error());
        CHECK(c.value().vertex_input == vertex_input);
        CHECK(layout_text(c.value().vertex_attributes) == k_mesh_attributes);
    }
}
} // namespace

TEST("sgl entry interface - a vertex entry point states every attribute it reads, a matrix's and an array's a piece "
     "each (EMIT-268)")
{
    check_mesh_layout(cc::format("{}{}", k_mesh_vertex, vertex_entry_of("mesh_vertex")), {}, "mesh_vertex", true);
}

TEST("sgl entry interface - the attributes match what the text declares, on dx12 and on vulkan (EMIT-268)")
{
    auto const source = cc::format("{}{}", k_mesh_vertex, vertex_entry_of("mesh_vertex"));
    auto const dx12 = sgl::compile_to_text({.source = source, .entry_point = "fed", .target = target::hlsl_dx12});
    auto const vulkan = sgl::compile_to_text({.source = source, .entry_point = "fed", .target = target::hlsl_vulkan});
    REQUIRE(dx12.has_value());
    REQUIRE(vulkan.has_value());
    // each attribute's semantic at its index, `UV1`, and its location, as the two texts declare the pieces
    CHECK(dx12.value().text.contains("float2 uv_1 : UV1;"));
    CHECK(dx12.value().text.contains("float4 frame_3 : FRAME3;"));
    CHECK(vulkan.value().text.contains("[[vk::location(4)]] float2 uv_1 : UV1;"));
    CHECK(vulkan.value().text.contains("[[vk::location(9)]] float scale : SCALE;"));
    // and as the declaring file's own description has its members, the struct a host generates its C++ type from
    auto const d = sgl::describe({.source = source, .source_name = "t.sgl"});
    REQUIRE(d.has_value());
    auto const* declared = static_cast<sgl::described_struct const*>(nullptr);
    for (auto const& s : d.value().structs)
        if (s.name == "mesh_vertex")
            declared = &s;
    REQUIRE(declared != nullptr);
    for (auto const& a : dx12.value().vertex_attributes)
        for (auto const& member : declared->members)
            if (member.name == a.name)
            {
                CHECK(a.semantic == member.semantic);
                CHECK(a.location == member.location + a.semantic_index);
                CHECK(a.format == member.format);
                CHECK(a.stream == member.stream);
                CHECK(a.is_per_instance == member.is_per_instance);
                CHECK(a.type == (member.pieces > 0 ? member.piece : member.type));
            }
}

TEST("sgl entry interface - a module's vertex input states its attributes where the module declares it (EMIT-268)")
{
    // a used module, by the module's name rather than the alias
    auto const meshes_source = cc::format("module meshes\n{}", k_mesh_vertex);
    sgl::library_file const meshes[] = {{.name = "meshes.sgl", .source = meshes_source}};
    SECTION("used")
    {
        check_mesh_layout(cc::format("use meshes as m\n{}", vertex_entry_of("m.mesh_vertex")), meshes,
                          "meshes.mesh_vertex", false);
    }
    // a dotted module, which `use a.b` binds as `b` (CHK-347)
    auto const dotted_source = cc::format("module sv.meshes\n{}", k_mesh_vertex);
    sgl::library_file const dotted[] = {{.name = "sv/meshes.sgl", .source = dotted_source}};
    SECTION("dotted")
    {
        check_mesh_layout(cc::format("use sv.meshes\n{}", vertex_entry_of("meshes.mesh_vertex")), dotted,
                          "sv.meshes.mesh_vertex", false);
    }
    // another file of the module the source joins, whose struct the source names bare as its own module's
    SECTION("joined")
    {
        check_mesh_layout(cc::format("module meshes\n{}", vertex_entry_of("mesh_vertex")), meshes, "mesh_vertex", false);
    }
}

TEST("sgl entry interface - every other entry point states no vertex attribute (EMIT-268)")
{
    // a vertex entry point that draws from no vertex buffer, and a stage of another kind
    auto const d = sgl::describe({.source = "struct varyings:\n    @position position: hpos4\n"
                                            "@vertex fun bare(@vertex_index i: int) -> varyings:\n"
                                            "    return { position = hpos4(float(i), 0.0, 0.0, 1.0) }\n"
                                            "@compute(1) fun work(@thread_id id: int3):\n    let x = id.x\n",
                                  .source_name = "t.sgl"});
    if (d.has_error())
        FAIL(d.error());
    for (auto const& e : d.value().entry_points)
    {
        CHECK(e.vertex_input.empty());
        CHECK(e.vertex_attributes.empty());
    }
}
