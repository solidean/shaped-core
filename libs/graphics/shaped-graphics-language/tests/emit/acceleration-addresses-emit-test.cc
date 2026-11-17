#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::native_route;
using sgl::emit::target;

// Every vulkan trace of SGL's reads its acceleration structure's address from `sg_acceleration_addresses`, which sg lays
// out and fills, and a compile lists that block among the slots it declares (EMIT-272): wherever SGL's own SPIR-V or
// the text declares it, used where a trace reads it, and on no other target.

namespace
{
constexpr native_route k_routes[] = {native_route::text, native_route::require};

/// One entry point of one source.
struct entry_of
{
    cc::string_view source;
    cc::string_view entry;
};

/// Pipeline traces from a raygen and from a closest hit, a miss that lists the structure and traces nothing, and a
/// compute stage that lists none.
/// `scene.second` is the third acceleration member the raygen lists.
constexpr auto k_pipeline = cc::string_view(R"(require raytracing_pipeline

struct radiance:
    color: float

rays rs:
    primary: radiance
    bounce: radiance

binding frame:
    world: acceleration_structure[.triangles]
    output: mut buffer[float]

binding scene:
    first: acceleration_structure[.triangles]
    second: acceleration_structure[.triangles]

binding data:
    values: mut buffer[float]

@raygen fun start(@launch_id id: int3){frame, scene}:
    let mut p = radiance(0.0)
    trace(scene.second, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), rs.primary, mut p)
    frame.output[id.x] = p.color

@miss fun sky(p: mut radiance){frame}:
    p.color = frame.output[0]

@miss fun far(p: mut radiance):
    p.color = 2.0

@closest_hit fun shade(h: triangle_hit, p: mut radiance){frame}:
    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 1.0, 0.0)), rs.bounce, mut p)

@compute(1) fun plain(@thread_id id: int3){data}:
    data.values[id.x] = 1.0

hit_group lit for rs:
    primary = (closest_hit = shade)

@raytracing pipeline path:
    rays = rs
    raygen = start
    miss.primary = sky
    miss.bounce = far
    hit_groups = (lit)
)");

/// A compute stage's ray query, and one that lists the structure and traces nothing.
constexpr auto k_query = cc::string_view(R"(require ray_query

binding frame:
    world: acceleration_structure[.triangles]
    output: mut buffer[float]

@compute(1) fun query(@thread_id id: int3){frame}:
    let h = frame.world.trace(ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), flags = ray_flags.force_opaque)
    frame.output[id.x] = h.t

@compute(1) fun listed(@thread_id id: int3){frame}:
    frame.output[id.x] = 1.0
)");

[[nodiscard]] sgl::emitted_source compiled(cc::string_view source, cc::string_view entry, target t, native_route route)
{
    auto const r = sgl::compile_to_text({.source = source, .entry_point = entry, .target = t, .native = route});
    if (r.has_error())
        FAIL(r.error());
    return r.value();
}

/// The acceleration-addresses slots `e` lists.
[[nodiscard]] cc::vector<sgl::interface_binding> address_slots_of(sgl::emitted_source const& e)
{
    auto found = cc::vector<sgl::interface_binding>();
    for (auto const& b : e.bindings)
        if (b.reserved == sgl::reserved_slot::acceleration_addresses)
            found.push_back(b);
    return found;
}

/// That `e`, compiled by `route` on vulkan, lists the block once as EMIT-272 states it, used as `is_used` says, and
/// lays its array out at byte 0 under the name the output reads it by.
void check_address_slot(sgl::emitted_source const& e, native_route route, bool is_used)
{
    auto const is_native = route != native_route::text;
    CHECK(e.spirv.empty() == !is_native);
    auto const slots = address_slots_of(e);
    REQUIRE(slots.size() == 1);
    auto const& slot = slots[0];
    CHECK(slot.is_reserved);
    CHECK(slot.name == "acceleration_addresses");
    // the text's cbuffer, which DXC reflects it by, and SGL's own SPIR-V's global
    auto const global
        = is_native ? cc::string_view("sg_acceleration_addresses") : cc::string_view("sg_acceleration_addresses_block");
    CHECK(slot.emitted == global);
    CHECK(slot.kind == sgl::described_member_kind::constant);
    CHECK(!slot.is_inline);
    CHECK(!slot.is_file_sampler);
    // vulkan's set 3, binding 22, and 16 addresses of 8 bytes, `uint4[8]`
    CHECK(slot.group == 3);
    CHECK(slot.slot == 22);
    CHECK(slot.count == 1);
    CHECK(slot.block_size == 128);
    CHECK(slot.is_used == is_used);
    auto layouts = 0;
    for (auto const& l : e.layouts)
        if (l.global == global)
        {
            ++layouts;
            CHECK(l.stride == 0);
            REQUIRE(l.fields.size() == 1);
            CHECK(l.fields[0].name == (is_native ? "values" : "sg_acceleration_addresses"));
            CHECK(l.fields[0].offset == 0);
        }
    CHECK(layouts == 1);
    // listed exactly where the text declares it
    if (!is_native)
        CHECK(e.text.contains("[[vk::binding(22, 3)]] cbuffer sg_acceleration_addresses_block\n{\n"
                              "    uint4 sg_acceleration_addresses[8];\n};\n"));
}

/// That `e` lists no address slot and lays out no address block.
void check_no_address_slot(sgl::emitted_source const& e)
{
    CHECK(address_slots_of(e).empty());
    for (auto const& l : e.layouts)
    {
        CHECK(l.global != "sg_acceleration_addresses");
        CHECK(l.global != "sg_acceleration_addresses_block");
    }
    CHECK(!e.text.contains("sg_acceleration_addresses"));
}
} // namespace

TEST("sgl emit - a vulkan trace lists sg_acceleration_addresses at set 3, binding 22, used (EMIT-272)")
{
    entry_of const traced[] = {{k_pipeline, "start"}, {k_pipeline, "shade"}, {k_query, "query"}};
    for (auto const& x : traced)
        for (auto const route : k_routes)
            SECTION("{} {}", x.entry, route == native_route::text ? "text" : "native")
            {
                check_address_slot(compiled(x.source, x.entry, target::hlsl_vulkan, route), route, true);
            }
}

TEST("sgl emit - an entry point that lists a structure and traces nothing lists the block where its output declares it "
     "(EMIT-272)")
{
    // the text declares it wherever the entry point lists an acceleration member, SGL's own SPIR-V only once a trace
    // reads it, so the text lists it unused and the binary not at all
    entry_of const listing[] = {{k_pipeline, "sky"}, {k_query, "listed"}};
    for (auto const& x : listing)
        SECTION("{}", x.entry)
        {
            check_address_slot(compiled(x.source, x.entry, target::hlsl_vulkan, native_route::text), native_route::text,
                               false);
            check_no_address_slot(compiled(x.source, x.entry, target::hlsl_vulkan, native_route::require));
        }
}

TEST("sgl emit - an entry point that lists no acceleration structure lists no address block (EMIT-272)")
{
    for (auto const route : k_routes)
        check_no_address_slot(compiled(k_pipeline, "plain", target::hlsl_vulkan, route));
}

TEST("sgl emit - dx12 traces through the structure's descriptor and lists no address block (EMIT-272)")
{
    entry_of const every[] = {{k_pipeline, "start"}, {k_pipeline, "shade"}, {k_query, "query"}, {k_pipeline, "sky"}};
    for (auto const& x : every)
        for (auto const route : k_routes)
            SECTION("{} {}", x.entry, route == native_route::text ? "text" : "native")
            {
                auto const e = compiled(x.source, x.entry, target::hlsl_dx12, route);
                CHECK(address_slots_of(e).empty());
                for (auto const& b : e.bindings)
                    CHECK(!b.is_reserved);
            }
}

TEST("sgl emit - a member's address position is its rank among the listed acceleration members (EMIT-272)")
{
    // the host's rule: the bindings of kind acceleration_structure and count 1, not reserved, by group and then slot,
    // the address of position k at `uint4` k / 2, `.xy` for an even k and `.zw` for an odd one
    auto const e = compiled(k_pipeline, "start", target::hlsl_vulkan, native_route::text);
    auto members = cc::vector<sgl::interface_binding>();
    for (auto const& b : e.bindings)
        if (b.kind == sgl::described_member_kind::acceleration_structure && b.count == 1 && !b.is_reserved)
        {
            auto at = isize(0);
            while (at < members.size()
                   && (members[at].group < b.group || (members[at].group == b.group && members[at].slot < b.slot)))
                ++at;
            members.insert_at(at, b);
        }
    REQUIRE(members.size() == 3);
    CHECK(members[2].name == "scene.second");
    // the raygen traces `scene.second`, position 2: the second vector's low half
    CHECK(e.text.contains("sg_acceleration_from_address(sg_acceleration_addresses[1].xy)"));
}
