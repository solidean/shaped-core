#include "check-test-support.hh"

using namespace sgl_test;

// CHK-282: a barrier, and a call that takes derivatives, stand where every invocation of the group arrives together.
// The pass judges the tree an emitter prints, and must be sound for every target; agreeing with Tint is no goal.

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

/// A compute entry point over workgroup memory: an atomic, and an array the threads share.
cc::string shared(cc::string_view body)
{
    return cc::format("@workgroup binding shared:\n"
                      "    hits: atomic[int]\n"
                      "    vals: int[8]\n"
                      "\n"
                      "binding work:\n"
                      "    enabled: bool\n"
                      "    output: mut buffer[int]\n"
                      "\n"
                      "@compute(64) fun cs(@thread_id id: int3){{shared, work}}:\n"
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

    // a helper's return from inside its loop leaves the loop, and then the block, in some threads only
    CHECK(reports_for(compute("    work.output[id.x] = find(id.x)\n"
                              "    workgroup_barrier()\n")
                      + "\nfun find(x: int) -> int:\n"
                        "    for i in 0 ..< 8:\n"
                        "        if i == x => return i\n"
                        "    return 8\n")
              .contains(kind));
}

TEST("sgl check - a divergent jump out of a case or a loop body reaches past the barrier")
{
    // a `case` arm is a branch on its scrutinee
    CHECK(reports_for(compute("    case id.x:\n"
                              "        0 => workgroup_barrier()\n"
                              "        _ => work.output[0] = 1\n"))
              .contains(kind));
    CHECK(reports_for(compute("    case work.count:\n"
                              "        0 => workgroup_barrier()\n"
                              "        _ => work.output[0] = 1\n"))
          == "");

    // the threads that continue skip the rest of this iteration's body
    CHECK(reports_for(compute("    for i in 0 ..< work.count:\n"
                              "        if id.x == i => continue\n"
                              "        workgroup_barrier()\n"))
              .contains(kind));

    // a `break` in a `case` arm leaves the loop, not the case
    CHECK(reports_for(compute("    for i in 0 ..< work.count:\n"
                              "        case id.x:\n"
                              "            0 => break\n"
                              "            _ => work.output[i] = 1\n"
                              "    workgroup_barrier()\n"))
              .contains(kind));
}

TEST("sgl check - workgroup memory and an atomic's result differ between threads")
{
    CHECK(reports_for(shared("    workgroup_barrier()\n")) == "");

    // whatever was stored to it, another thread may have stored something else
    auto const read = reports_for(shared("    for i in 0 ..< shared.vals[0]:\n"
                                         "        work.output[i] = 1\n"
                                         "    workgroup_barrier()\n"));
    CHECK(read.contains(kind));
    CHECK(read.contains("is read from shared.vals, workgroup memory the threads write"));

    // an atomic's memory is read first, so the note names the read rather than the result
    CHECK(reports_for(shared("    if shared.hits.add(1) == 0 => workgroup_barrier()\n")).contains(kind));

    // the right side of an `or` with an effect becomes an `if`, which runs only where the left side is false
    auto const synced = cc::string("\nfun synced() -> bool:\n    workgroup_barrier()\n    return true\n");
    CHECK(reports_for(shared("    if id.x < 4 or synced() => work.output[0] = 1\n") + synced).contains(kind));
    CHECK(reports_for(shared("    if work.enabled or synced() => work.output[0] = 1\n") + synced) == "");
}

TEST("sgl check - a `while` condition is tested again by the invocations a divergent break left behind")
{
    // the first test runs in every pixel, the second only in those that did not break
    CHECK(reports_for(pixel("    let mut x = 1.0\n"
                            "    while ddx(x) < 1.0:\n"
                            "        if p.uv.x < 0.5 => break\n"
                            "        c.x += 1.0\n"))
              .contains("ddx takes derivatives"));
    CHECK(reports_for(pixel("    let mut x = 1.0\n"
                            "    while ddx(x) < 1.0:\n"
                            "        if material.threshold < 0.5 => break\n"
                            "        c.x += 1.0\n"))
          == "");
    // and a condition that differs itself leaves some pixels behind at every test
    CHECK(reports_for(pixel("    while ddx(c.x) < p.uv.x:\n"
                            "        c.x += 1.0\n"))
              .contains("ddx takes derivatives"));
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

TEST("sgl check - a dynamic index into a binding array is proven uniform, or marked nonuniform")
{
    auto const indexed = [](cc::string_view line)
    {
        return reports_for(cc::format("require binding_arrays\n\n"
                                      "binding materials:\n"
                                      "    @sampler(smp) albedo: texture_2d[float4][8]\n"
                                      "    smp: sampler\n"
                                      "    slot: int\n"
                                      "\n"
                                      "struct pixel_input:\n"
                                      "    @position position: hpos4\n"
                                      "    uv: float2\n"
                                      "    @interpolate(.flat) material: int\n"
                                      "\n"
                                      "@pixel struct target:\n"
                                      "    color: float4\n"
                                      "\n"
                                      "@pixel fun ps(p: pixel_input){{materials}} -> target:\n"
                                      "    return {{color = {}}}\n",
                                      line));
    };
    CHECK(indexed("materials.albedo[materials.slot].sample(p.uv)") == "");
    CHECK(indexed("materials.albedo[3].sample(p.uv)") == "");
    CHECK(indexed("materials.albedo[nonuniform p.material].sample(p.uv)") == "");

    // CHK-300: forgetting the mark reads another invocation's texture on some hardware, silently
    auto const unmarked = indexed("materials.albedo[p.material].sample(p.uv)");
    CHECK(unmarked.contains("non-uniform-index"));
    CHECK(unmarked.contains("mark it `nonuniform p.material`, or make it the same in all of them"));
    CHECK(unmarked.contains("this value comes from the stage's input struct"));

    // and a mark where none is needed pays for nothing
    auto const needless = indexed("materials.albedo[nonuniform materials.slot].sample(p.uv)");
    CHECK(needless.contains("needless-nonuniform"));
}

TEST("sgl check - an index into a binding array that a parameter stands for is judged where the caller wrote it")
{
    auto const passed = [](cc::string_view line)
    {
        return reports_for(cc::format("require binding_arrays\n\n"
                                      "binding post:\n"
                                      "    arr: mut image_2d[.r32_float][4]\n"
                                      "\n"
                                      "fun read_it(a: mut image_2d[.r32_float], xy: int2) -> float => a.load(xy) + "
                                      "a.load(xy)\n"
                                      "\n"
                                      "@compute(8, 8) fun cs(@thread_id id: int3){{post}}:\n"
                                      "    let xy = id.xy\n"
                                      "    let j = id.z\n"
                                      "    post.arr[0].store(xy, read_it({}, xy))\n",
                                      line));
    };
    CHECK(passed("post.arr[nonuniform j]") == "");
    CHECK(passed("post.arr[nonuniform (j + 1)]") == "");

    // the mark the hint asks for is written at the call, which is the one place it can be; a parameter named twice
    // is still one index
    CHECK(passed("post.arr[j]")
          == "non-uniform-index user:[j] an index into a binding array that may differ between invocations: mark it "
             "`nonuniform j`, or make it the same in all of them\n"
             "  note user:[id] this value comes from the stage input id\n");
    CHECK(passed("post.arr[j + 1]")
          == "non-uniform-index user:[j + 1] an index into a binding array that may differ between invocations: mark "
             "it `nonuniform (j + 1)`, or make it the same in all of them\n"
             "  note user:[id] this value comes from the stage input id\n");
}

TEST("sgl check - an image the shader also stores to differs between threads, named alone or in a binding array")
{
    auto const images = [](cc::string_view load)
    {
        return reports_for(cc::format("require binding_arrays\n\n"
                                      "binding work:\n"
                                      "    one: mut image_2d[.r32_float]\n"
                                      "    many: mut image_2d[.r32_float][4]\n"
                                      "\n"
                                      "@compute(8, 8) fun cs(@thread_id id: int3){{work}}:\n"
                                      "    let v = {}\n"
                                      "    if v > 0.5 => workgroup_barrier()\n",
                                      load));
    };
    CHECK(images("work.one.load(int2(0, 0))").contains("is loaded from work.one, which the shader also stores to"));
    CHECK(images("work.many[1].load(int2(0, 0))").contains("is loaded from work.many, which the shader also stores to"));
}

TEST("sgl check - `nonuniform i` stands only as the index into a binding array")
{
    auto const marked = [](cc::string_view body)
    {
        return reports_for(cc::format("require binding_arrays\n\n"
                                      "binding materials:\n"
                                      "    texs: texture_2d[float4][8]\n"
                                      "\n"
                                      "binding results:\n"
                                      "    values: mut buffer[float4]\n"
                                      "\n"
                                      "@compute(64) fun cs(@thread_id id: int3){{materials, results}}:\n"
                                      "{}",
                                      body));
    };
    CHECK(marked("    results.values[id.x] = materials.texs[nonuniform id.x].load(int2(0, 0))\n") == "");

    // CHK-300: elsewhere the mark would ask a target for what it does not need, or for nothing at all
    constexpr auto misplaced = "`nonuniform i` marks an index into a binding array, and stands only as one";
    auto const in_let = marked("    let j = nonuniform id.x\n"
                               "    results.values[id.x] = materials.texs[j].load(int2(0, 0))\n");
    CHECK(in_let.contains("wrong-kind-of-name"));
    CHECK(in_let.contains(misplaced));
    CHECK(marked("    let xs = [1.0, 2.0]\n"
                 "    results.values[id.x] = float4(xs[nonuniform (id.x % 2)], 0.0, 0.0, 0.0)\n")
              .contains(misplaced));
    CHECK(marked("    results.values[nonuniform id.x] = float4(0.0, 0.0, 0.0, 0.0)\n").contains(misplaced));
    CHECK(marked("    results.values[id.x] = materials.texs[nonuniform (nonuniform id.x)].load(int2(0, 0))\n")
              .contains(misplaced));
}

TEST("sgl check - a marked index stays marked when an argument after it moves it into a local")
{
    auto const loaded = [](cc::string_view index)
    {
        return reports_for(cc::format("require binding_arrays\n\n"
                                      "binding materials:\n"
                                      "    texs: texture_2d[float4][4]\n"
                                      "\n"
                                      "binding results:\n"
                                      "    values: mut buffer[float4]\n"
                                      "\n"
                                      "fun pick(x: int) -> int:\n"
                                      "    if x == 0 => return 1\n"
                                      "    return 0\n"
                                      "\n"
                                      "@compute(64) fun cs(@thread_id id: int3, @workgroup_id g: int3){{materials, "
                                      "results}}:\n"
                                      "    results.values[id.x] = materials.texs[{}].load(int2(pick(id.x), 0))\n",
                                      index));
    };
    // `pick` returns early, so its value is computed ahead of the load, and the index ahead of that
    CHECK(loaded("nonuniform (id.x % 4)") == "");
    CHECK(loaded("id.x % 4").contains("non-uniform-index"));
    CHECK(loaded("nonuniform (g.x % 4)").contains("needless-nonuniform"));
}

TEST("sgl check - an if that is a value runs its branch in the flow its condition makes, and select runs no branch")
{
    // CHK-375: only the taken branch runs, so a derivative inside it is where not every pixel of the quad arrives
    CHECK(reports_for(pixel("    let d = if p.uv.x > 0.5 => ddx(p.uv.x) else 0.0\n    c.x = d\n")).contains(kind));
    CHECK(reports_for(pixel("    let w = if p.uv.x > 0.5 => fwidth(p.uv) else p.uv\n    c.x = w.y\n")).contains(kind));
    // a uniform condition leaves every pixel together
    CHECK(reports_for(pixel("    let d = if material.threshold > 0.5 => ddx(p.uv.x) else 0.0\n    c.x = d\n")) == "");
    // select evaluates all three of its arguments, so nothing in them is under its condition (CHK-365)
    CHECK(reports_for(pixel("    c.x = select(p.uv.x > 0.5, ddx(p.uv.x), 0.0)\n")) == "");
}
