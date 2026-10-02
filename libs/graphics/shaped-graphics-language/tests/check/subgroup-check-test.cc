#include "check-test-support.hh"

using namespace sgl_test;

// The subgroup operations, the subgroup's stage inputs and the preferred size (CHK-371, CHK-376 to CHK-379), and the
// uniform load (CHK-374): what the corpus cannot say, since each is a refusal of an entry point.

namespace
{
/// A compute entry point over a buffer it reads, one it writes and workgroup memory, with `body` as its body.
cc::string compute(cc::string_view body, cc::string_view require = "require subgroups\n\n")
{
    return cc::format("{}"
                      "binding work:\n"
                      "    input: buffer[float]\n"
                      "    output: mut buffer[float]\n"
                      "    counter: mut buffer[atomic[uint]]\n"
                      "\n"
                      "@workgroup binding spd:\n"
                      "    finished: uint\n"
                      "    hits: atomic[uint]\n"
                      "    slots: uint[4]\n"
                      "    corner: uint2\n"
                      "\n"
                      "@compute(64) fun cs(@thread_id id: int3, @local_thread_index li: int){{work, spd}}:\n"
                      "{}",
                      require, body);
}

constexpr auto non_uniform = "non-uniform-control-flow";
} // namespace

TEST("sgl check - a subgroup operation needs subgroups of a device, and so does each of its stage inputs")
{
    // CHK-376 and CHK-322: the call is what needs it
    CHECK(reports_for(compute("    work.output[li] = subgroup_add(work.input[li])\n", ""))
              .contains("feature-not-declared user:[cs] cs needs subgroups"));
    CHECK(reports_for(compute("    work.output[li] = subgroup_add(work.input[li])\n")) == "");

    // CHK-272: in every stage that takes them, the compute stage as much as the pixel stage
    constexpr auto sized = "binding work:\n"
                           "    output: mut buffer[float]\n"
                           "\n"
                           "@compute(64) fun cs(@local_thread_index li: int, @subgroup_size n: int){work}:\n"
                           "    work.output[li] = n as float\n";
    CHECK(reports_for(sized).contains("feature-not-declared user:[cs] cs needs subgroups"));
    CHECK(reports_for(cc::format("require subgroups\n\n{}", sized)) == "");
    constexpr auto edges = "struct vout:\n    @position p: hpos4\n\n@pixel struct target:\n    c: float4\n\n";
    CHECK(reports_for(cc::string(edges)
                      + "@pixel fun ps(v: vout, @subgroup_invocation_id lane: int) -> target:\n"
                        "    return { c = float4(lane as float, 0.0, 0.0, 1.0) }\n")
              .contains("feature-not-declared user:[ps] ps needs subgroups"));
    // a vertex stage has no subgroup to name
    CHECK(reports_for(cc::string("require subgroups\n\n") + edges
                      + "@vertex fun vs(@subgroup_size n: int) -> vout:\n"
                        "    return { p = hpos4(n as float, 0.0, 0.0, 1.0) }\n")
              .contains("@subgroup_size is an input of the pixel and compute stages"));
}

TEST("sgl check - a subgroup operation stands in uniform control flow, and what it gives differs within a workgroup")
{
    // CHK-377: judged as a barrier is
    CHECK(reports_for(compute("    let total = subgroup_add(work.input[li])\n"
                              "    if total > 0.0 => workgroup_barrier()\n"))
              .contains(non_uniform));
    auto const branched = reports_for(compute("    if id.x > 3:\n"
                                              "        work.output[li] = quad_swap_x(work.input[li])\n"));
    CHECK(branched.contains(non_uniform));
    CHECK(branched.contains("quad_swap_x exchanges values within the subgroup, and not every invocation of it reaches "
                            "it here"));
    // a uniform branch keeps the subgroup together, whatever the operation's argument is
    CHECK(reports_for(compute("    if work.input[0] > 0.0:\n"
                              "        work.output[li] = subgroup_max(work.input[li])\n"))
          == "");
}

TEST("sgl check - a lane a target takes as a constant is a constant within the subgroup or the quad")
{
    // CHK-378, which the corpus states for the accepted forms
    auto const lane
        = [](cc::string_view call) { return reports_for(compute(cc::format("    work.output[li] = {}\n", call))); };
    CHECK(lane("subgroup_broadcast(work.input[li], 127)") == "");
    CHECK(lane("subgroup_broadcast(work.input[li], li)").contains("invalid-constant-argument"));
    CHECK(lane("subgroup_broadcast(work.input[li], 128)")
              .contains("the lane of subgroup_broadcast is a constant from "
                        "0 to 127"));
    CHECK(lane("quad_broadcast(work.input[li], 4)").contains("the lane of quad_broadcast is a constant from 0 to 3"));
    // a shuffle computes its lane at run time
    CHECK(lane("subgroup_shuffle(work.input[li], li + 1)") == "");
    // a shuffle by a mask or a delta takes it as the broadcast takes its lane
    CHECK(lane("subgroup_shuffle_xor(work.input[li], 16)") == "");
    CHECK(lane("subgroup_shuffle_xor(work.input[li], li)")
              .contains("the mask of subgroup_shuffle_xor is a constant from 0 to 127"));
    CHECK(lane("subgroup_shuffle_up(work.input[li], li)")
              .contains("the delta of subgroup_shuffle_up is a constant from 0 to 127"));
    CHECK(lane("subgroup_shuffle_down(work.input[li], -1)").contains("invalid-constant-argument"));
}

TEST("sgl check - workgroup_uniform_load makes a branch on workgroup memory uniform")
{
    // CHK-374: the shape a single-pass reduction ends in, the last workgroup alone going on
    constexpr auto tail = "    if li == 0:\n"
                          "        spd.finished = work.counter[0].add(1u)\n"
                          "    if {} == 7u:\n"
                          "        workgroup_barrier()\n";
    CHECK(reports_for(compute(cc::format(tail, "workgroup_uniform_load(spd.finished)"))) == "");
    // a plain read is different in every thread, whatever was stored
    auto const plain = reports_for(compute(cc::format(tail, "spd.finished")));
    CHECK(plain.contains(non_uniform));
    CHECK(plain.contains("is read from spd.finished, workgroup memory the threads write"));
    // and the load itself is a barrier
    CHECK(reports_for(compute("    if id.x > 3:\n"
                              "        work.output[li] = workgroup_uniform_load(spd.finished) as float\n"))
              .contains("workgroup_uniform_load waits for every thread of the workgroup"));
}

TEST("sgl check - workgroup_uniform_load takes every type select takes, a 16-bit one with its feature")
{
    // CHK-374 and CHK-382
    constexpr auto source = "{}"
                            "binding work:\n"
                            "    output: mut buffer[float]\n"
                            "\n"
                            "@workgroup binding spd:\n"
                            "    scale: half2\n"
                            "    count: ushort\n"
                            "\n"
                            "@compute(64) fun cs(@local_thread_index li: int){{work, spd}}:\n"
                            "    let s = workgroup_uniform_load(spd.scale)\n"
                            "    let n = workgroup_uniform_load(spd.count)\n"
                            "    work.output[li] = (s.x as float) + ((n as uint) as float)\n";
    CHECK(reports_for(cc::format(source, "require shader_f16\nrequire shader_int16\n\n")) == "");
    auto const lacking = reports_for(cc::format(source, ""));
    CHECK(lacking.contains("feature-not-declared"));
    CHECK(!lacking.contains("no-matching-overload"));
}

TEST("sgl check - workgroup_uniform_load reads a member of workgroup memory, and nothing else")
{
    auto const loaded = [](cc::string_view argument)
    { return reports_for(compute(cc::format("    let v = workgroup_uniform_load({})\n", argument))); };
    CHECK(loaded("spd.corner") == "");
    constexpr auto refused = "wrong-kind-of-name";
    CHECK(loaded("work.input[0]").contains(refused));
    CHECK(loaded("spd.slots[1]").contains("unsupported-yet"));
    CHECK(loaded("spd.corner.x").contains("unsupported-yet"));
    CHECK(loaded("spd.hits").contains(refused));
    CHECK(reports_for(compute("    let mut v = 1u\n"
                              "    let w = workgroup_uniform_load(v)\n"))
              .contains(refused));
    CHECK(reports_for(cc::string("binding consts:\n    n: uint\n\n")
                      + compute("    let v = workgroup_uniform_load(consts.n)\n"))
              .contains(refused));
}

TEST("sgl check - a preferred subgroup size is a power of two from 4 to 128, on a compute entry point")
{
    // CHK-371: a literal or an int const, and no feature: a preference never refuses a device
    auto const preferred = [](cc::string_view argument)
    {
        return reports_for(cc::format("const wave = 64\n"
                                      "\n"
                                      "binding work:\n"
                                      "    output: mut buffer[float]\n"
                                      "\n"
                                      "@compute(64) @preferred_subgroup_size{} fun cs(@thread_id id: int3){{work}}:\n"
                                      "    work.output[id.x] = 1.0\n",
                                      argument));
    };
    CHECK(preferred("(32)") == "");
    CHECK(preferred("(wave)") == "");
    CHECK(preferred("(4)") == "");
    CHECK(preferred("(128)") == "");
    constexpr auto refused = "invalid-attribute-arguments";
    CHECK(preferred("(2)").contains(refused));
    CHECK(preferred("(256)").contains(refused));
    CHECK(preferred("(48)").contains(refused));
    CHECK(preferred("").contains(refused));
    CHECK(preferred("(32.0)").contains(refused));
    CHECK(reports_for("struct vout:\n    @position p: hpos4\n\n"
                      "@preferred_subgroup_size(32) @vertex fun vs() -> vout:\n"
                      "    return { p = hpos4(0.0, 0.0, 0.0, 1.0) }\n")
              .contains("@preferred_subgroup_size asks for the subgroups of a compute entry point"));
}

TEST("sgl check - a test that reaches a subgroup operation is unsupported, and its uniform load is a read")
{
    // CHK-379: a run is one invocation, which has no subgroup
    CHECK(reports_for("test subgroup_add(1.0) == 1.0\n")
              .contains("unsupported-yet user:[subgroup_add(1.0)] a test that reaches subgroup_add"));
    CHECK(reports_for("@workgroup binding spd:\n"
                      "    finished: uint\n"
                      "\n"
                      "test:\n"
                      "    spd.finished = 5u\n"
                      "    workgroup_uniform_load(spd.finished) == 5u\n")
          == "");
}

TEST("sgl check - a compute stage forms its quads along one axis, four invocations at a time")
{
    // CHK-380: HLSL's quads of a two-dimensional workgroup are squares of its threads' ids, which no other target forms
    auto const shaped = [](cc::string_view workgroup, cc::string_view call)
    {
        return reports_for(cc::format("require subgroups\n"
                                      "\n"
                                      "binding work:\n"
                                      "    output: mut buffer[float]\n"
                                      "\n"
                                      "@compute{} fun cs(@local_thread_index li: int){{work}}:\n"
                                      "    work.output[li] = {}\n",
                                      workgroup, call));
    };
    CHECK(shaped("(64)", "quad_swap_y(li as float)") == "");
    CHECK(shaped("(8, 8)", "quad_swap_x(li as float)")
              .contains("invalid-entry-point user:[quad_swap_x(li as float)] quad_swap_x forms quads along one axis"));
    CHECK(shaped("(6)", "quad_broadcast(li as float, 1)").contains("cs's is (6, 1, 1)"));
    // the rest of the family takes the subgroup whole, whatever the workgroup's shape
    CHECK(shaped("(8, 8)", "subgroup_add(li as float)") == "");
}
