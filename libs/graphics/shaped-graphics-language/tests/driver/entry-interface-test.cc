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

TEST("sgl entry interface - a module's vertex input is named as the program names it")
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
