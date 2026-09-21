#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
constexpr cc::string_view k_edges = "struct pixel_input:\n"
                                    "    @position position: hpos4\n"
                                    "\n"
                                    "@pixel struct frame:\n"
                                    "    color: float4\n"
                                    "\n";

/// One read-only buffer, one writable one, and an entry point that touches both.
constexpr cc::string_view k_buffers = "binding work:\n"
                                      "    src: buffer[float]\n"
                                      "    dst: mut buffer[float]\n"
                                      "\n"
                                      "@pixel fun main_ps(p: pixel_input){work} -> frame:\n"
                                      "    let v = work.src[0]\n"
                                      "    work.dst[1] = v * 2.0\n"
                                      "    return {color = float4(v, v, v, 1.0)}\n";

cc::string text_of(cc::string_view rest, target t)
{
    auto const e = emit_source(cc::string(k_edges) + rest, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}

/// The errors of entry point 0 for one target, since a buffer is written for three of the four.
cc::string errors_for(cc::string_view rest, target t)
{
    return sgl::emit::dump_errors(emit_source(cc::string(k_edges) + rest, 0, t));
}
} // namespace

TEST("sgl emit - a buffer is a storage array in WGSL, addressed by its place in the binding list")
{
    auto const wgsl = text_of(k_buffers, target::wgsl);
    CHECK(wgsl.contains("@group(0) @binding(0) var<storage, read> work_src: array<f32>;\n"));
    CHECK(wgsl.contains("@group(0) @binding(1) var<storage, read_write> work_dst: array<f32>;\n"));
    CHECK(wgsl.contains("    let v: f32 = work_src[0];\n"));
    CHECK(wgsl.contains("    work_dst[1] = v * 2.0;\n"));
}

TEST("sgl emit - a buffer is a structured buffer in HLSL, and slib's pass writes its register")
{
    auto const hlsl = text_of(k_buffers, target::hlsl_dx12);
    // The group number is the one address SGL writes; every register is the binding pass's (the spec's bindings file).
    CHECK(hlsl.contains("#pragma sc group 0\n"));
    CHECK(hlsl.contains("namespace work_bindings\n"));
    CHECK(hlsl.contains("    StructuredBuffer<float> work_src;\n"));
    CHECK(hlsl.contains("    RWStructuredBuffer<float> work_dst;\n"));
    CHECK(!hlsl.contains("register("));
    CHECK(hlsl.contains("work_bindings::work_dst[1] = v * 2.0;\n"));
}

TEST("sgl emit - the group number is the binding's place in the entry point's list")
{
    constexpr auto two = "binding frame_data:\n"
                         "    a: buffer[float]\n"
                         "\n"
                         "binding work:\n"
                         "    b: buffer[float]\n"
                         "\n"
                         "@pixel fun main_ps(p: pixel_input){frame_data, work} -> frame:\n"
                         "    let v = frame_data.a[0] + work.b[0]\n"
                         "    return {color = float4(v, v, v, 1.0)}\n";
    auto const wgsl = text_of(two, target::wgsl);
    CHECK(wgsl.contains("@group(0) @binding(0) var<storage, read> frame_data_a: array<f32>;\n"));
    CHECK(wgsl.contains("@group(1) @binding(0) var<storage, read> work_b: array<f32>;\n"));
}

TEST("sgl emit - an @inline binding takes no group, and stands last")
{
    constexpr auto both = "binding work:\n"
                          "    values: buffer[float]\n"
                          "\n"
                          "@inline binding tuning:\n"
                          "    scale: float\n"
                          "\n"
                          "@pixel fun main_ps(p: pixel_input){work, tuning} -> frame:\n"
                          "    let v = work.values[0] * tuning.scale\n"
                          "    return {color = float4(v, v, v, 1.0)}\n";
    // The buffer keeps group 0: the inline constants are addressed by sg and counted by nobody.
    CHECK(text_of(both, target::wgsl).contains("@group(0) @binding(0) var<storage, read> work_values: array<f32>;\n"));

    constexpr auto wrong_order = "binding work:\n"
                                 "    values: buffer[float]\n"
                                 "\n"
                                 "@inline binding tuning:\n"
                                 "    scale: float\n"
                                 "\n"
                                 "@pixel fun main_ps(p: pixel_input){tuning, work} -> frame:\n"
                                 "    let v = work.values[0] * tuning.scale\n"
                                 "    return {color = float4(v, v, v, 1.0)}\n";
    CHECK(errors_for(wrong_order, target::wgsl)
          == "unsupported an @inline binding that is not the last of the list: 'tuning'\n");
}

TEST("sgl emit - MSL declines a buffer rather than writing text no compiler takes")
{
    // A Metal buffer is an argument of the kernel, not a global, which this writer does not build yet.
    CHECK(errors_for(k_buffers, target::msl)
          == "unsupported a buffer binding, which MSL takes as an entry-point argument\n");
    CHECK(errors_for(k_buffers, target::hlsl_vulkan) == "");
}

TEST("sgl emit - a buffer's identifier is minted like any other name, and the text says what the host calls it")
{
    // Nothing binds by the identifier, so a local keeps its name and the buffer is the one minted.
    auto const e = emit_source(cc::string(k_edges)
                                   + "binding work:\n"
                                     "    dst: mut buffer[float]\n"
                                     "\n"
                                     "@pixel fun main_ps(p: pixel_input){work} -> frame:\n"
                                     "    let work_dst = 2.0\n"
                                     "    work.dst[0] = work_dst\n"
                                     "    return {color = float4(1.0, 1.0, 1.0, 1.0)}\n",
                               0, target::wgsl);
    REQUIRE(sgl::emit::dump_errors(e) == "");
    CHECK(e.text.contains("var<storage, read_write> work_dst_1: array<f32>;"));
    CHECK(e.text.contains("let work_dst: f32 = 2.0;"));
    REQUIRE(e.bound_names.size() == 1);
    CHECK(e.bound_names[0].emitted == "work_dst_1");
    CHECK(e.bound_names[0].host == "work.dst");
}

TEST("sgl emit - two buffers whose identifiers would collide both reach the host under their own paths")
{
    // `a_b.c` and `a.b_c` both want the identifier `a_b_c`; the host never sees it.
    auto const e = emit_source(cc::string(k_edges)
                                   + "binding a_b:\n"
                                     "    c: buffer[float]\n"
                                     "\n"
                                     "binding a:\n"
                                     "    b_c: buffer[float]\n"
                                     "    scale: float\n"
                                     "\n"
                                     "@pixel fun main_ps(p: pixel_input){a_b, a} -> frame:\n"
                                     "    let v = a_b.c[0] + a.b_c[0] * a.scale\n"
                                     "    return {color = float4(v, v, v, 1.0)}\n",
                               0, target::wgsl);
    REQUIRE(sgl::emit::dump_errors(e) == "");
    auto hosts = cc::vector<cc::string>();
    auto emitted = cc::vector<cc::string>();
    for (auto const& b : e.bound_names)
    {
        hosts.push_back(b.host);
        emitted.push_back(b.emitted);
    }
    // The group's block first, under the binding's own name, then the buffers in group order.
    REQUIRE(hosts.size() == 3);
    CHECK(hosts[0] == "a");
    CHECK(hosts[1] == "a_b.c");
    CHECK(hosts[2] == "a.b_c");
    CHECK(emitted[1] != emitted[2]);
}

namespace
{
/// A helper with a loop, so a call of it is a block that legalizing moves in front of its statement.
constexpr cc::string_view k_pick = "binding work:\n"
                                   "    values: mut buffer[float]\n"
                                   "\n"
                                   "fun pick(i: int) -> int:\n"
                                   "    let mut j = i\n"
                                   "    while j > 3:\n"
                                   "        j -= 4\n"
                                   "    return j\n"
                                   "\n";
} // namespace

TEST("sgl emit - an index that inlines a helper is evaluated in front, the place's before the value's")
{
    // EVAL-14: the place's index first, then the value.
    auto const hlsl = text_of(cc::string(k_pick)
                                  + "@pixel fun main_ps(p: pixel_input){work} -> frame:\n"
                                    "    work.values[pick(1)] = work.values[pick(2)]\n"
                                    "    return {color = float4(1.0, 1.0, 1.0, 1.0)}\n",
                              target::hlsl_dx12);
    CHECK(!hlsl.contains("[]"));
    auto const place = hlsl.find("int j = 1;");
    auto const value = hlsl.find("int j_1 = 2;");
    CHECK(place >= 0);
    CHECK(value > place);
    CHECK(hlsl.contains("work_bindings::work_values[j] = work_bindings::work_values[j_1];\n"));
}

TEST("sgl emit - `op=` on a buffer element evaluates its index once")
{
    auto const wgsl = text_of(cc::string(k_pick)
                                  + "@pixel fun main_ps(p: pixel_input){work} -> frame:\n"
                                    "    work.values[pick(5)] += 2.0\n"
                                    "    return {color = float4(1.0, 1.0, 1.0, 1.0)}\n",
                              target::wgsl);
    // One loop, not two: the read goes through the local the store's index was evaluated into.
    CHECK(wgsl.find("while") == wgsl.rfind("while"));
    CHECK(wgsl.contains("    work_values[index] = work_values[index] + 2.0;\n"));
}

TEST("sgl emit - an element read to the left of a helper that stores to its buffer is read first")
{
    // A store is an effect: the read on the left is pinned before the helper's body moves in front of it.
    auto const wgsl = text_of("binding work:\n"
                              "    values: mut buffer[float]\n"
                              "\n"
                              "fun bump(i: int){work} -> float:\n"
                              "    work.values[i] = work.values[i] + 1.0\n"
                              "    return 0.0\n"
                              "\n"
                              "@pixel fun main_ps(p: pixel_input){work} -> frame:\n"
                              "    let a = work.values[0] + bump(0)\n"
                              "    return {color = float4(a, a, a, 1.0)}\n",
                              target::wgsl);
    auto const pin = wgsl.find("let values_before: f32 = work_values[0];");
    auto const store = wgsl.find("work_values[0] = work_values[0] + 1.0;");
    CHECK(pin >= 0);
    CHECK(store > pin);
    CHECK(wgsl.contains("let a: f32 = values_before + 0.0;"));
}

TEST("sgl emit - a shadowing local is a local of its own in the text")
{
    // CHK-53: each `let x` is its own local, so the second is minted and the first is still what its value reads.
    auto const wgsl = text_of("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                              "    let x = p.position.x\n"
                              "    let x = x * 2.0\n"
                              "    return {color = float4(x, x, x, 1.0)}\n",
                              target::wgsl);
    CHECK(wgsl.contains("    let x: f32 = p.position.x;\n"));
    CHECK(wgsl.contains("    let x_1: f32 = x * 2.0;\n"));
    CHECK(wgsl.contains("vec4f(x_1, x_1, x_1, 1.0)"));
}
