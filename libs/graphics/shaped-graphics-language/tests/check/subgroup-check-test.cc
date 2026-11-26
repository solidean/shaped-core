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

TEST("sgl check - a test reaches a subgroup operation where its lanes arrive together, and its uniform load is a read")
{
    // CHK-379: a test runs as one lane per invocation of a subgroup (EVAL-140), held to the uniformity an entry point is
    CHECK(reports_for("require subgroups\n"
                      "test subgroup_add(1.0) == test_lane_count() as float\n")
          == "");
    CHECK(reports_for("require subgroups\n"
                      "test:\n"
                      "    if test_lane() == 0:\n"
                      "        let one = subgroup_add(1.0)\n"
                      "        one == 1.0\n"
                      "    true\n")
              .contains("non-uniform-control-flow"));
    // CHK-512: what only a test states, no entry point reaches
    CHECK(reports_for("@compute(1) fun cs():\n"
                      "    let lane = test_lane()\n")
              .contains("stage-not-allowed user:[test_lane()] a compute entry point reaches this, and only a test "
                        "calls test_lane"));
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
    CHECK(shaped("(6)", "quad_broadcast(li as float, 1)").contains("one reaching it has (6, 1, 1)"));
    // the rest of the family takes the subgroup whole, whatever the workgroup's shape
    CHECK(shaped("(8, 8)", "subgroup_add(li as float)") == "");
}

TEST("sgl check - a branch on quad_any or quad_all keeps a derivative in uniform control flow, and nothing more "
     "(CHK-533)")
{
    auto const pixel = [](cc::string_view body, cc::string_view require = "require subgroups, quad_control\n\n")
    {
        return cc::format("{}"
                          "binding res:\n"
                          "    t: texture_2d[float4]\n"
                          "    smp: sampler\n"
                          "\n"
                          "struct vary:\n"
                          "    @position position: hpos4\n"
                          "    uv: float2\n"
                          "\n"
                          "@pixel struct target:\n"
                          "    color: float4\n"
                          "\n"
                          "@pixel fun ps(v: vary){{res}} -> target:\n"
                          "{}"
                          "    return {{color = float4(0.0)}}\n",
                          require, body);
    };

    // every invocation of the quad goes one way, so the quad reaches the sample whole
    CHECK(reports_for(pixel("    if quad_any(v.uv.x > 0.5):\n"
                            "        return {color = res.t.sample(v.uv, res.smp)}\n"))
          == "");
    CHECK(reports_for(pixel("    let near = quad_all(v.uv.y < 0.25)\n"
                            "    if near:\n"
                            "        return {color = float4(ddx(v.uv.x)) + float4(quad_swap_x(v.uv.y))}\n"))
          == "");
    // a value set under such a branch, and one that joins a quad vote with a uniform value, stays the quad's
    CHECK(reports_for(pixel("    let mut level = 0.0\n"
                            "    if quad_any(v.uv.x > 0.5):\n"
                            "        level = 1.0\n"
                            "    if level > 0.5 and quad_all(v.uv.y >= 0.0):\n"
                            "        return {color = res.t.sample(v.uv, res.smp)}\n"))
          == "");

    // a subgroup's vote leaves a helper free to go the other way, as WGSL's spec says it may
    CHECK(reports_for(pixel("    if subgroup_any(v.uv.x > 0.5):\n"
                            "        return {color = res.t.sample(v.uv, res.smp)}\n"))
              .contains(non_uniform));
    // a quad's agreement is no subgroup's, so a subgroup operation under it still diverges
    CHECK(reports_for(pixel("    if quad_any(v.uv.x > 0.5):\n"
                            "        return {color = float4(subgroup_add(v.uv.x))}\n"))
              .contains(non_uniform));
    // a value that differs within the quad, joined with a quad vote, differs within the quad
    CHECK(reports_for(pixel("    if quad_any(v.uv.x > 0.5) and v.uv.y > 0.5:\n"
                            "        return {color = res.t.sample(v.uv, res.smp)}\n"))
              .contains(non_uniform));
    // and a quad vote in control flow that differs within the quad reaches part of it
    CHECK(reports_for(pixel("    if v.uv.y > 0.5:\n"
                            "        if quad_any(v.uv.x > 0.5):\n"
                            "            return {color = res.t.sample(v.uv, res.smp)}\n"))
              .contains(non_uniform));

    // the votes are their feature's, which WebGPU has on no device
    CHECK(reports_for(pixel("    if quad_any(v.uv.x > 0.5):\n"
                            "        return {color = float4(1.0)}\n",
                            "require subgroups\n\n"))
              .contains("needs quad_control"));
    // and a pixel stage's: a compute stage's quads have no helpers to agree with
    CHECK(reports_for(compute("    work.output[id.x] = if quad_any(work.input[id.x] > 0.0) => 1.0 else 0.0\n",
                              "require subgroups, quad_control\n\n"))
              .contains("stage-not-allowed"));
}

TEST("sgl check - a branch on what the subgroup agrees on keeps a subgroup operation in uniform control flow, and "
     "nothing more (CHK-537)")
{
    // every active invocation of the subgroup goes one way, so the subgroup reaches the sum whole
    CHECK(reports_for(compute("    if subgroup_any(work.input[li] > 0.0):\n"
                              "        work.output[li] = subgroup_add(work.input[li])\n"))
          == "");
    // a reduction and a broadcast of a constant lane agree as a vote does, and a branch nested in one stays the subgroup's
    CHECK(reports_for(compute("    let total = subgroup_add(work.input[li])\n"
                              "    if total > subgroup_broadcast(work.input[li], 0):\n"
                              "        if subgroup_all(work.input[li] < 4.0):\n"
                              "            work.output[li] = subgroup_max(work.input[li])\n"))
          == "");
    // a value set under such a branch differs between subgroups alone too
    CHECK(reports_for(compute("    let mut level = 0.0\n"
                              "    if subgroup_any(work.input[li] > 0.0):\n"
                              "        level = 1.0\n"
                              "    if level > 0.5:\n"
                              "        work.output[li] = subgroup_add(work.input[li])\n"))
          == "");

    // a barrier waits for the whole workgroup, whose subgroups may go different ways
    CHECK(reports_for(compute("    if subgroup_any(work.input[li] > 0.0):\n"
                              "        workgroup_barrier()\n"))
              .contains(non_uniform));
    // a quad operation is refused too: a helper may sit out the vote and go the other way
    CHECK(reports_for(compute("    if subgroup_any(work.input[li] > 0.0):\n"
                              "        work.output[li] = quad_swap_x(work.input[li])\n"))
              .contains(non_uniform));
    // a value that differs within the subgroup, joined with a vote, differs within the subgroup
    CHECK(reports_for(compute("    if subgroup_any(work.input[li] > 0.0) and li > 3:\n"
                              "        work.output[li] = subgroup_add(work.input[li])\n"))
              .contains(non_uniform));
    // and a vote in control flow that differs within the subgroup is itself refused
    CHECK(reports_for(compute("    if li > 3:\n"
                              "        if subgroup_any(work.input[li] > 0.0):\n"
                              "            work.output[li] = subgroup_add(work.input[li])\n"))
              .contains(non_uniform));
    // a scan, a shuffle and an election give each invocation its own, so a branch on one diverges
    CHECK(reports_for(compute("    if subgroup_exclusive_add(1) == 0:\n"
                              "        work.output[li] = subgroup_add(work.input[li])\n"))
              .contains(non_uniform));
    CHECK(reports_for(compute("    if subgroup_elect():\n"
                              "        work.output[li] = subgroup_add(work.input[li])\n"))
              .contains(non_uniform));
}

TEST("sgl check - a subgroup's agreement keeps no derivative, and a quad's with a subgroup's keeps neither (CHK-537)")
{
    auto const pixel = [](cc::string_view body)
    {
        return cc::format("require subgroups, quad_control\n"
                          "\n"
                          "binding res:\n"
                          "    t: texture_2d[float4]\n"
                          "    smp: sampler\n"
                          "\n"
                          "struct vary:\n"
                          "    @position position: hpos4\n"
                          "    uv: float2\n"
                          "\n"
                          "@pixel struct target:\n"
                          "    color: float4\n"
                          "\n"
                          "@pixel fun ps(v: vary){{res}} -> target:\n"
                          "{}"
                          "    return {{color = float4(0.0)}}\n",
                          body);
    };

    // a pixel stage's subgroup operation stands under a subgroup's vote, as a compute stage's does
    CHECK(reports_for(pixel("    if subgroup_any(v.uv.x > 0.5):\n"
                            "        return {color = float4(subgroup_add(v.uv.x))}\n"))
          == "");
    // a derivative does not: the helpers of the quad may sit out the vote
    CHECK(reports_for(pixel("    if subgroup_any(v.uv.x > 0.5):\n"
                            "        return {color = float4(ddx(v.uv.x))}\n"))
              .contains(non_uniform));

    // a quad's agreement says nothing of the other quads of the subgroup, and a subgroup's nothing of the helpers
    constexpr auto both = "    let q = quad_any(v.uv.x > 0.5)\n"
                          "    let s = subgroup_any(v.uv.y > 0.5)\n"
                          "    if q and s:\n"
                          "        return {{color = {}}}\n";
    CHECK(reports_for(pixel(cc::format(both, "float4(subgroup_add(v.uv.x))"))).contains(non_uniform));
    CHECK(reports_for(pixel(cc::format(both, "res.t.sample(v.uv, res.smp)"))).contains(non_uniform));
    CHECK(reports_for(pixel(cc::format(both, "float4(quad_swap_x(v.uv.x))"))).contains(non_uniform));
    // each alone keeps what it keeps
    CHECK(reports_for(pixel("    let q = quad_any(v.uv.x > 0.5)\n"
                            "    if q:\n"
                            "        return {color = res.t.sample(v.uv, res.smp)}\n"))
          == "");
    // and a local the two set in turn is neither's
    CHECK(reports_for(pixel("    let mut k = 0.0\n"
                            "    if quad_any(v.uv.x > 0.5):\n"
                            "        k = 1.0\n"
                            "    if subgroup_any(v.uv.y > 0.5):\n"
                            "        k = 2.0\n"
                            "    if k > 0.5:\n"
                            "        return {color = float4(subgroup_add(v.uv.x)) + res.t.sample(v.uv, res.smp)}\n"))
              .contains(non_uniform));
}

TEST("sgl check - subgroup_elect needs subgroups, and what it gives differs within the subgroup")
{
    // CHK-376
    CHECK(reports_for(compute("    if subgroup_elect() => work.counter[0].add(1u)\n", ""))
              .contains("feature-not-declared user:[cs] cs needs subgroups"));
    CHECK(reports_for(compute("    if subgroup_elect() => work.counter[0].add(1u)\n")) == "");
    CHECK(reports_for(compute("    if li > 3:\n"
                              "        work.output[li] = if subgroup_elect() => 1.0 else 0.0\n"))
              .contains(non_uniform));
}

TEST("sgl check - the subgroup operations but the quad ones stand in the raygen, closest-hit, miss and callable stages")
{
    // CHK-376, CHK-568: one feature, whatever the stage; the any-hit and intersection stages, and the quad operations,
    // stay without them, as do the subgroup's stage inputs (CHK-570)
    auto const pipeline
        = [](cc::string_view raygen_body, cc::string_view any_hit_body = "    return hit_decision.accept\n")
    {
        return reports_for(cc::format("require raytracing_pipeline, subgroups\n"
                                      "binding frame:\n"
                                      "    world: acceleration_structure[.triangles]\n"
                                      "    counts: mut buffer[atomic[uint]]\n"
                                      "struct radiance:\n"
                                      "    t: float\n"
                                      "rays path_rays:\n"
                                      "    surface: radiance\n"
                                      "@raygen fun primary(@launch_id id: int3){{frame}}:\n"
                                      "{}"
                                      "    let mut p = radiance(0.0)\n"
                                      "    trace(frame.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, "
                                      "0.0, 1.0)), path_rays.surface, mut p)\n"
                                      "@miss fun sky(p: mut radiance){{frame}}:\n"
                                      "    let misses = subgroup_add(1u)\n"
                                      "    if subgroup_elect() => frame.counts[1].add(misses)\n"
                                      "@closest_hit fun shade(h: triangle_hit, p: mut radiance){{frame}}:\n"
                                      "    p.t = subgroup_max(h.t)\n"
                                      "@any_hit fun decide(c: triangle_candidate, p: mut radiance) -> hit_decision:\n"
                                      "{}"
                                      "hit_group shaded for path_rays:\n"
                                      "    surface = (closest_hit = shade, any_hit = decide)\n"
                                      "@raytracing pipeline path:\n"
                                      "    rays = path_rays\n"
                                      "    raygen = primary\n"
                                      "    miss.surface = sky\n"
                                      "    hit_groups = (shaded)\n",
                                      raygen_body, any_hit_body));
    };
    auto const accepted = pipeline("    let most = subgroup_max(id.x as uint)\n"
                                   "    if subgroup_elect() => frame.counts[0].max(most)\n");
    CHECK(accepted == "").dump("report", accepted);
    auto const quad = pipeline("    let x = quad_swap_x(id.x)\n");
    CHECK(quad.contains("stage-not-allowed")).dump("report", quad);
    auto const any_hit = pipeline("", "    if subgroup_any(c.t > 1.0) => return hit_decision.ignore\n"
                                      "    return hit_decision.accept\n");
    CHECK(any_hit.contains("stage-not-allowed")).dump("report", any_hit);
    auto const lane = reports_for("require raytracing_pipeline, subgroups\n"
                                  "binding frame:\n"
                                  "    counts: mut buffer[atomic[uint]]\n"
                                  "@raygen fun primary(@launch_id id: int3, @subgroup_size n: int){frame}:\n"
                                  "    frame.counts[0].add(n as uint)\n");
    CHECK(lane != "").dump("report", lane);
}
