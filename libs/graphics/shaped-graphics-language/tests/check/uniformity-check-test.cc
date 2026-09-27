#include "check-test-support.hh"

using namespace sgl_test;

// CHK-282: a barrier, and a call that takes derivatives, stand where every invocation of the group arrives together.
// The rules are WGSL's, applied to the tree an emitter prints, so that Tint accepts whatever the check pass does.

namespace
{
/// A compute entry point over a binding with a constant, a buffer it reads and a buffer it writes.
cc::string compute(cc::string_view body)
{
    return cc::format("binding work:\n"
                      "    count: int\n"
                      "    enabled: bool\n"
                      "    input: buffer[int]\n"
                      "    output: mut buffer[int]\n"
                      "\n"
                      "@compute(64) fun cs(@thread_id id: int3, @workgroup_id g: int3){{work}}:\n"
                      "{}",
                      body);
}

/// A pixel entry point that samples a texture, with `body` ahead of its return.
cc::string pixel(cc::string_view body)
{
    return cc::format("binding material:\n"
                      "    @sampler(smp) albedo: texture_2d[float4]\n"
                      "    smp: sampler\n"
                      "    threshold: float\n"
                      "\n"
                      "struct pixel_input:\n"
                      "    @position position: hpos4\n"
                      "    uv: float2\n"
                      "\n"
                      "@pixel struct target:\n"
                      "    color: float4\n"
                      "\n"
                      "@pixel fun ps(p: pixel_input){{material}} -> target:\n"
                      "    let mut c = float4(0.0, 0.0, 0.0, 1.0)\n"
                      "{}"
                      "    return {{color = c}}\n",
                      body);
}

constexpr auto kind = "non-uniform-control-flow";
} // namespace

TEST("sgl check - a barrier stands where every thread of the workgroup arrives")
{
    CHECK(reports_for(compute("    workgroup_barrier()\n")) == "");
    // a constant, a workgroup's id and a read-only buffer at a uniform index are the same in every thread
    CHECK(reports_for(compute("    if work.enabled => storage_barrier()\n")) == "");
    CHECK(reports_for(compute("    if g.x > 3 => texture_barrier()\n")) == "");
    CHECK(reports_for(compute("    if work.input[0] > 3 => workgroup_barrier()\n")) == "");
    CHECK(reports_for(compute("    for i in 0 ..< work.count:\n        workgroup_barrier()\n")) == "");

    // the classic hang: threads past the end leave, and the others wait for them
    auto const early = reports_for(compute("    if id.x >= work.count => return\n    workgroup_barrier()\n"));
    CHECK(early.contains(kind));
    CHECK(early.contains("workgroup_barrier waits for every thread of the workgroup, and not every one reaches it "
                         "here"));
    // what follows a leaving `if` is its `else` in the tree an emitter prints, so the branch is what the note names
    CHECK(early.contains("note user:[if id.x >= work.count => return] this branch tests a value that comes from the "
                         "stage input id"));

    auto const branched = reports_for(compute("    if id.x < 10 => workgroup_barrier()\n"));
    CHECK(branched.contains(kind));
    CHECK(branched.contains("this branch tests a value that comes from the stage input id"));

    // what the shader writes may differ by the time it is read, and so may what a divergent branch assigned
    CHECK(reports_for(compute("    if work.output[0] > 3 => workgroup_barrier()\n"))
              .contains("is read from work.output, which the shader also writes"));
    CHECK(reports_for(compute("    let mut n = 0\n"
                              "    if id.x < 10 => n = 1\n"
                              "    if n > 0 => workgroup_barrier()\n"))
              .contains("is set where the control flow differs"));
}

TEST("sgl check - a loop that some threads leave early is divergent all through, and after it")
{
    // the leaving thread misses the next iteration's barrier, even the one above the `break`
    auto const inside = reports_for(compute("    for i in 0 ..< work.count:\n"
                                            "        workgroup_barrier()\n"
                                            "        if id.x == i => break\n"));
    CHECK(inside.contains(kind));
    CHECK(reports_for(compute("    for i in 0 ..< work.count:\n"
                              "        if id.x == i => break\n"
                              "    workgroup_barrier()\n"))
              .contains(kind));
    CHECK(reports_for(compute("    for i in 0 ..< id.x:\n        workgroup_barrier()\n")).contains(kind));

    // a helper's early return is a divergent exit of the block it became
    CHECK(reports_for(compute("    if first(id.x) => workgroup_barrier()\n")
                      + "\nfun first(x: int) -> bool:\n"
                        "    if x == 0 => return true\n"
                        "    return false\n")
              .contains(kind));
    // and one called under a uniform condition is fine
    CHECK(reports_for(compute("    if work.enabled => sync()\n") + "\nfun sync():\n    workgroup_barrier()\n") == "");
}

TEST("sgl check - a sample that picks its own level stands where every pixel of the quad arrives")
{
    CHECK(reports_for(pixel("    c = material.albedo.sample(p.uv)\n")) == "");
    CHECK(reports_for(pixel("    if material.threshold > 0.5 => c = material.albedo.sample(p.uv)\n")) == "");
    // an explicit level takes no derivative
    CHECK(reports_for(pixel("    if p.uv.x < 0.5 => c = material.albedo.sample(p.uv, level = 0.0)\n")) == "");
    // a discard leaves the pixel a helper, which still takes part in its quad's derivatives
    CHECK(reports_for(pixel("    if p.uv.x < 0.1 => discard\n    c = material.albedo.sample(p.uv)\n")) == "");

    auto const varying = reports_for(pixel("    if p.uv.x < 0.5 => c = material.albedo.sample(p.uv)\n"));
    CHECK(varying.contains(kind));
    CHECK(varying.contains("sample takes derivatives across a quad of pixels, and not every pixel of the quad reaches "
                           "it here"));
    CHECK(varying.contains("this branch tests a value that comes from the stage's input struct"));

    CHECK(reports_for(pixel("    if p.uv.x < 0.5 => return {color = c}\n    c = material.albedo.sample(p.uv)\n"))
              .contains(kind));
    // the right side of an `and` runs only where the left one is true
    CHECK(reports_for(pixel("    if p.uv.x < 0.5 and material.albedo.sample(p.uv).x > 0.5 => c.x = 1.0\n")).contains(kind));
    CHECK(reports_for(pixel("    c.y = ddx(p.uv.x)\n")) == "");
    CHECK(reports_for(pixel("    if p.uv.y > 0.5 => c.y = ddy(p.uv.x)\n")).contains("ddy takes derivatives"));
}
