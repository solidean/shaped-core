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
    // the host's rule: k is the number of bindings of kind acceleration_structure, not reserved, before it by group and
    // then slot, a binding array one of them; the address of position k at `uint4` k / 2, `.xy` for an even k, `.zw`
    // for an odd one
    auto const e = compiled(k_pipeline, "start", target::hlsl_vulkan, native_route::text);
    auto members = cc::vector<sgl::interface_binding>();
    for (auto const& b : e.bindings)
        if (b.kind == sgl::described_member_kind::acceleration_structure && !b.is_reserved)
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

namespace
{
/// A binding array of structures before a member of its own binding, which a ray query traces.
constexpr auto k_after_array = cc::string_view(R"(require ray_query
require binding_arrays

binding frame:
    many: acceleration_structure[.triangles][2]
    world: acceleration_structure[.triangles]
    output: mut buffer[float]

@compute(1) fun query(@thread_id id: int3){frame}:
    let h = frame.world.trace(ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), flags = ray_flags.force_opaque)
    frame.output[id.x] = h.t
)");

/// A module's binding of two structures listed ahead of the program's own, a trace of each.
constexpr auto k_module_scenes = cc::string_view(R"(module scenes
require ray_query

binding sky:
    near: acceleration_structure[.triangles]
    far: acceleration_structure[.triangles]
)");
constexpr auto k_after_module = cc::string_view(R"(require ray_query
use scenes

binding frame:
    output: mut buffer[float]
    world: acceleration_structure[.triangles]

@compute(1) fun query(@thread_id id: int3){scenes.sky, frame}:
    let h = frame.world.trace(ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), flags = ray_flags.force_opaque)
    let g = scenes.sky.far.trace(ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), flags = ray_flags.force_opaque)
    frame.output[id.x] = h.t + g.t
)");

/// The address positions SGL's own SPIR-V reads, as its dump reads them: the `uint4` an access chain picks, and the
/// half its shuffle takes, components 0 and 1 for an even position, 2 and 3 for an odd one.
[[nodiscard]] cc::string native_positions_of(cc::string_view dump)
{
    auto result = cc::string();
    constexpr auto chain = cc::string_view("access_chain @sg_acceleration_addresses 0x0 0x");
    for (auto at = dump.find(chain); at != -1; at = dump.find(chain, at + 1))
    {
        auto const end = dump.find('\n', at);
        auto const vector = dump.subview({.start = at + chain.size(), .end = end});
        REQUIRE(vector.size() == 1);
        auto const shuffle = dump.find("= shuffle ", end);
        REQUIRE(shuffle != -1);
        auto const is_high = dump.subview({.start = shuffle, .end = dump.find('\n', shuffle)}).ends_with(" %2 %3");
        result += cc::format("{}{}", result.empty() ? "" : ", ", 2 * (vector[0] - '0') + (is_high ? 1 : 0));
    }
    return result;
}

/// `e`'s listed acceleration members in EMIT-272's order, by group and then by slot, each as `group:slot`, and a binding
/// array of structures with its `[count]`, which sg's vulkan layout counts as one position.
[[nodiscard]] cc::string address_order_of(sgl::emitted_source const& e)
{
    auto members = cc::vector<sgl::interface_binding>();
    for (auto const& b : e.bindings)
        if (b.kind == sgl::described_member_kind::acceleration_structure && !b.is_reserved)
        {
            auto at = isize(0);
            while (at < members.size()
                   && (members[at].group < b.group || (members[at].group == b.group && members[at].slot < b.slot)))
                ++at;
            members.insert_at(at, b);
        }
    auto names = cc::string();
    for (auto const& m : members)
        names += cc::format("{}{}:{}{}", names.empty() ? "" : ", ", m.group, m.slot,
                            m.count == 1 ? cc::string() : cc::format("[{}]", m.count));
    return names;
}
} // namespace

TEST("sgl emit - a binding array of structures takes one address position, as sg's vulkan layout counts it (EMIT-272)")
{
    // sg counts every acceleration binding of a layout, a binding array as one (sg::impl::acceleration_position), and
    // fills the address of the k-th at k; so the member after a binding array is position 1 on both routes, the
    // second address of the first `uint4`
    auto const text = compiled(k_after_array, "query", target::hlsl_vulkan, native_route::text);
    CHECK(address_order_of(text) == "0:0[2], 0:2");
    CHECK(text.text.contains("sg_acceleration_from_address(sg_acceleration_addresses[0].zw)"));
    CHECK(!text.text.contains("sg_acceleration_addresses[0].xy"));
    check_address_slot(text, native_route::text, true);
    auto const native = compiled(k_after_array, "query", target::hlsl_vulkan, native_route::require);
    CHECK(native_positions_of(native.text) == "1");
    check_address_slot(native, native_route::require, true);
}

TEST("sgl emit - address positions run over a module's binding by group, then by slot (EMIT-272)")
{
    // `scenes.sky` is group 0, so its `far` is position 1, and the program's `frame.world`, group 1, position 2
    sgl::library_file const library[] = {{.name = "scenes.sgl", .source = k_module_scenes}};
    for (auto const route : k_routes)
        SECTION("{}", route == native_route::text ? "text" : "native")
        {
            auto const r = sgl::compile_to_text({.source = k_after_module,
                                                 .library = library,
                                                 .entry_point = "query",
                                                 .target = target::hlsl_vulkan,
                                                 .native = route});
            if (r.has_error())
                FAIL(r.error());
            auto const& e = r.value();
            CHECK(address_order_of(e) == "0:0, 0:1, 1:1");
            if (route == native_route::text)
            {
                CHECK(e.text.contains("sg_acceleration_from_address(sg_acceleration_addresses[1].xy)"));
                CHECK(e.text.contains("sg_acceleration_from_address(sg_acceleration_addresses[0].zw)"));
            }
            else
                CHECK(native_positions_of(e.text) == "2, 1");
            check_address_slot(e, route, true);
        }
}
