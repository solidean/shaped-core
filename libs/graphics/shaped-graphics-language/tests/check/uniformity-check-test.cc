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

TEST("sgl check - a loop that some threads leave early is divergent all through, and a barrier after it too")
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

TEST("sgl check - a compare-exchange's result differs between threads, and its loop leaves the flow as it found it")
{
    // what either form read, and whether the weak one stored, is another thread's business as much as this one's
    CHECK(reports_for(shared("    if shared.hits.compare_exchange_strong(0, id.x) == 0 => workgroup_barrier()\n"))
              .contains(kind));
    CHECK(reports_for(shared("    let r = shared.hits.compare_exchange_weak(0, id.x)\n"
                             "    if r.exchanged => workgroup_barrier()\n"))
              .contains(kind));
    CHECK(reports_for(shared("    let mut r = shared.hits.compare_exchange_weak(0, id.x)\n"
                             "    while r.old == 0 and not r.exchanged:\n"
                             "        r = shared.hits.compare_exchange_weak(0, id.x)\n"
                             "        workgroup_barrier()\n"))
              .contains(kind));

    // the strong one's loop is the target's, inside the call, and every thread leaves it: a barrier after it holds
    CHECK(reports_for(shared("    let before = shared.hits.compare_exchange_strong(0, id.x)\n"
                             "    workgroup_barrier()\n"
                             "    work.output[id.x] = before\n"))
          == "");
}

TEST("sgl check - an index read from a compare-exchange is marked nonuniform")
{
    constexpr auto source = "require binding_arrays\n"
                            "\n"
                            "binding work:\n"
                            "    claims: mut buffer[atomic[int]]\n"
                            "    sources: buffer[int][4]\n"
                            "    output: mut buffer[int]\n"
                            "\n"
                            "@compute(64) fun cs(@thread_id id: int3){{work}}:\n"
                            "    let k = work.claims[0].{} & 3\n"
                            "    work.output[id.x] = work.sources[{}k][0]\n";
    for (auto const call : {"compare_exchange_strong(0, id.x)", "compare_exchange_weak(0, id.x).old"})
    {
        CHECK(reports_for(cc::format(source, call, "")).contains("non-uniform-index")).context(call);
        CHECK(reports_for(cc::format(source, call, "nonuniform ")) == "").context(call);
    }
}

TEST("sgl check - a pixel stage samples with derivatives after a strong compare-exchange")
{
    // its loop leaves every pixel of the quad where it found it, and only helpers skip it (EMIT-158)
    CHECK(reports_for("binding material:\n"
                      "    @sampler(smp) albedo: texture_2d[float4]\n"
                      "    smp: sampler\n"
                      "    claims: mut buffer[atomic[uint]]\n"
                      "\n"
                      "struct pixel_input:\n"
                      "    @position position: hpos4\n"
                      "    uv: float2\n"
                      "\n"
                      "@pixel struct target:\n"
                      "    color: float4\n"
                      "\n"
                      "@pixel fun ps(p: pixel_input){material} -> target:\n"
                      "    let before = material.claims[p.position.x as int].compare_exchange_strong(0u, 1u)\n"
                      "    return {color = material.albedo.sample(p.uv) * (before as float)}\n")
          == "");
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

TEST("sgl check - mut bytes differ between threads, named alone or in a binding array")
{
    auto const words = [](cc::string_view load)
    {
        return reports_for(cc::format("require binding_arrays\n\n"
                                      "binding work:\n"
                                      "    one: mut bytes\n"
                                      "    many: mut bytes[4]\n"
                                      "\n"
                                      "@compute(8, 8) fun cs(@thread_id id: int3){{work}}:\n"
                                      "    let v = {}\n"
                                      "    if v > 0u => workgroup_barrier()\n",
                                      load));
    };
    CHECK(words("work.one.load(0u)").contains("is loaded from work.one, which the shader also stores to"));
    CHECK(words("work.many[1].load(0u)").contains("is loaded from work.many, which the shader also stores to"));
}

TEST("sgl check - an index the pass proves neither the same nor different is marked `uniform` or `nonuniform`")
{
    auto const counted = [](cc::string_view index)
    {
        return reports_for(cc::format("require binding_arrays\n\n"
                                      "binding mats:\n"
                                      "    texs: texture_2d[float4][8]\n"
                                      "\n"
                                      "binding work:\n"
                                      "    counts: mut buffer[int]\n"
                                      "    values: mut buffer[float4]\n"
                                      "\n"
                                      "@compute(64) fun cs(@thread_id id: int3){{mats, work}}:\n"
                                      "    let k = work.counts[0] % 8\n"
                                      "    work.values[id.x] = mats.texs[{}].load(int2(0, 0))\n",
                                      index));
    };
    // CHK-394: the author's word, either way, where the pass has none
    CHECK(counted("uniform k") == "");
    CHECK(counted("nonuniform k") == "");
    CHECK(counted("k")
          == "unproven-index user:[k] an index into a binding array that the compiler cannot prove the same in every "
             "invocation: mark it `uniform k` where it is, or `nonuniform k` where it may differ\n"
             "  note user:[work.counts[0]] this value is read from work.counts, which the shader also writes\n");
}

TEST("sgl check - a `uniform` mark on an index the pass proves varying is refused")
{
    auto const picked = [](cc::string_view index)
    {
        return reports_for(cc::format("require binding_arrays\n\n"
                                      "binding mats:\n"
                                      "    @sampler(smp) texs: texture_2d[float4][8]\n"
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
                                      "@pixel fun ps(p: pixel_input){{mats}} -> target:\n"
                                      "    return {{color = mats.texs[{}].sample(p.uv)}}\n",
                                      index));
    };
    // a mark that costs nothing is no needless one
    CHECK(picked("uniform mats.slot") == "");
    CHECK(picked("uniform p.material")
          == "uniform-index-varies user:[uniform p.material] this index is marked `uniform`, and it differs between "
             "invocations: mark it `nonuniform p.material` instead\n"
             "  note user:[p] this value comes from the stage's input struct\n");
}

TEST("sgl check - a helper reached with a uniform index and a varying one is split, or marked `nonuniform`")
{
    auto const helped = [](cc::string_view index)
    {
        return reports_for(
            cc::format("require binding_arrays\n\n"
                       "binding mats:\n"
                       "    @sampler(smp) texs: texture_2d[float4][8]\n"
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
                       "fun sample_map(map: int, uv: float2){{mats}} -> float4 => mats.texs[{}].sample(uv)\n"
                       "\n"
                       "@pixel fun ps(p: pixel_input){{mats}} -> target:\n"
                       "    return {{color = sample_map(mats.slot, p.uv) + sample_map(p.material, p.uv)}}\n",
                       index));
    };
    // CHK-395: the explicit mark is the author's choice to pay on the uniform call too
    CHECK(helped("nonuniform (map % 8)") == "");
    CHECK(helped("map % 8")
          == "mixed-index user:[map % 8] some calls give this index into a binding array a value the same in every "
             "invocation, and others one that may differ: split sample_map in two, one of which marks it `nonuniform "
             "(map % 8)`, or mark it `nonuniform (map % 8)` here and pay for the mark on the uniform calls too\n"
             "  note user:[sample_map(mats.slot, p.uv)] this call gives it a value the same in every invocation\n"
             "  note user:[sample_map(p.material, p.uv)] and this one a value that may differ between them\n"
             "  note user:[p] this value comes from the stage's input struct\n");
    CHECK(helped("uniform (map % 8)").contains("uniform-index-varies"));
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
    CHECK(marked("    let j = uniform id.x\n"
                 "    results.values[id.x] = materials.texs[nonuniform j].load(int2(0, 0))\n")
              .contains("`uniform i` marks an index into a binding array, and stands only as one: `t[uniform i]`"));
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

namespace
{
/// `stage` behind a ray set, and a binding with a binding array of bytes, which every ray-tracing stage here indexes.
cc::string ray_stage(cc::string_view stage)
{
    return cc::format("require raytracing_pipeline\n"
                      "require binding_arrays\n\n"
                      "struct radiance:\n"
                      "    color: float3\n\n"
                      "struct operand:\n"
                      "    x: float\n\n"
                      "struct sphere:\n"
                      "    radius: float\n\n"
                      "rays path_rays:\n"
                      "    surface: radiance\n\n"
                      "binding tables:\n"
                      "    world: acceleration_structure[.triangles]\n"
                      "    buffers: bytes[8]\n"
                      "    output: mut buffer[float]\n\n"
                      "{}",
                      stage);
}

/// The diagnostic a ray-tracing stage's unmarked `index` gets, with the note on what made it differ.
cc::string unmarked(cc::string_view index, cc::string_view note)
{
    return cc::format("non-uniform-index user:[{0}] an index into a binding array that may differ between invocations: "
                      "mark it `nonuniform ({0})`, or make it the same in all of them\n"
                      "  note {1}\n",
                      index, note);
}
} // namespace

TEST("sgl check - what a ray-tracing stage is handed differs between its invocations, but the launch's size")
{
    // CHK-283: the lanes of one wave hit different instances and primitives, so an index made of a hit must be marked
    auto const hit = [](cc::string_view index)
    {
        return reports_for(ray_stage(cc::format("@closest_hit fun shade(h: triangle_hit, p: mut radiance){{tables}}:\n"
                                                "    p.color.x = tables.buffers[{}].load(0u) as float\n",
                                                index)));
    };
    CHECK(hit("nonuniform (h.instance_id % 8)") == "");
    CHECK(hit("h.instance_id % 8")
          == unmarked("h.instance_id % 8", "user:[h] this value comes from what the stage is handed"));
    CHECK(hit("h.primitive_index % 8").contains("non-uniform-index"));

    auto const candidate = [](cc::string_view index)
    {
        return reports_for(
            ray_stage(cc::format("@any_hit fun cut(c: triangle_candidate, p: mut radiance){{tables}} -> hit_decision:\n"
                                 "    if tables.buffers[{}].load(0u) == 0u => return hit_decision.ignore\n"
                                 "    return hit_decision.accept\n",
                                 index)));
    };
    CHECK(candidate("nonuniform (c.primitive_index % 8)") == "");
    CHECK(candidate("c.primitive_index % 8").contains("non-uniform-index"));

    auto const box = [](cc::string_view index)
    {
        return reports_for(ray_stage(cc::format("@intersection fun boxed(b: procedural_box){{tables}} -> "
                                                "report[sphere]:\n"
                                                "    let t = tables.buffers[{}].load(0u) as float\n"
                                                "    return report(t = t, attributes = sphere(1.0))\n",
                                                index)));
    };
    CHECK(box("nonuniform (b.instance_id % 8)") == "");
    CHECK(box("b.instance_id % 8").contains("non-uniform-index"));

    // each invocation of a miss traced a ray of its own
    auto const missed = [](cc::string_view index)
    {
        return reports_for(ray_stage(cc::format("@miss fun sky(p: mut radiance, r: ray){{tables}}:\n"
                                                "    p.color.x = tables.buffers[{}].load(0u) as float\n",
                                                index)));
    };
    CHECK(missed("nonuniform ((r.direction.x as int) % 8)") == "");
    CHECK(missed("(r.direction.x as int) % 8")
          == unmarked("(r.direction.x as int) % 8", "user:[r] this value comes from what the stage is handed"));

    // a raygen's launch index is its own, and the launch's size every invocation's
    auto const launched = [](cc::string_view index)
    {
        return reports_for(ray_stage(cc::format("@raygen fun primary(@launch_id id: int3, @launch_size size: "
                                                "int3){{tables}}:\n"
                                                "    tables.output[id.x] = tables.buffers[{}].load(0u) as float\n",
                                                index)));
    };
    CHECK(launched("nonuniform (id.x % 8)") == "");
    CHECK(launched("id.x % 8") == unmarked("id.x % 8", "user:[id] this value comes from what the stage is handed"));
    CHECK(launched("size.x % 8") == "");
    CHECK(launched("nonuniform (size.x % 8)").contains("needless-nonuniform"));
}

TEST("sgl check - what a trace or a callable writes back differs between invocations, whatever ray was traced")
{
    // the shaders the trace runs read what their own invocation is handed, its launch index among it
    auto const traced = [](cc::string_view index)
    {
        return reports_for(ray_stage(cc::format("@raygen fun primary(@launch_id id: int3){{tables}}:\n"
                                                "    let mut p = radiance(float3(0.0, 0.0, 0.0))\n"
                                                "    trace(tables.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = "
                                                "vec3(0.0, 0.0, 1.0)), "
                                                "path_rays.surface, mut p)\n"
                                                "    tables.output[id.x] = tables.buffers[{}].load(0u) as float\n",
                                                index)));
    };
    CHECK(traced("nonuniform ((p.color.x as int) % 8)") == "");
    CHECK(traced("(p.color.x as int) % 8")
          == unmarked("(p.color.x as int) % 8",
                      "user:[trace(tables.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), "
                      "path_rays.surface, mut p)] this value is what the shaders the call ran wrote back"));

    auto const called = [](cc::string_view index)
    {
        return reports_for(ray_stage(cc::format("@callable fun doubled(v: mut operand):\n"
                                                "    v.x = v.x * 2.0\n\n"
                                                "callables ops = (doubled)\n\n"
                                                "@raygen fun primary(@launch_id id: int3){{tables}}:\n"
                                                "    let mut v = operand(1.0)\n"
                                                "    ops[0](mut v)\n"
                                                "    tables.output[id.x] = tables.buffers[{}].load(0u) as float\n",
                                                index)));
    };
    CHECK(called("nonuniform ((v.x as int) % 8)") == "");
    CHECK(called("(v.x as int) % 8").contains("this value is what the shaders the call ran wrote back"));
}

TEST("sgl check - a payload that is a member of a local leaves that local differing between invocations")
{
    auto const traced = [](cc::string_view index)
    {
        return reports_for(ray_stage(cc::format("struct carried:\n"
                                                "    p: radiance\n"
                                                "    weight: float\n\n"
                                                "@raygen fun primary(@launch_id id: int3){{tables}}:\n"
                                                "    let mut s = carried(radiance(float3(0.0, 0.0, 0.0)), 1.0)\n"
                                                "    trace(tables.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = "
                                                "vec3(0.0, 0.0, 1.0)), "
                                                "path_rays.surface, mut s.p)\n"
                                                "    tables.output[id.x] = tables.buffers[{}].load(0u) as float\n",
                                                index)));
    };
    CHECK(traced("nonuniform ((s.p.color.x as int) % 8)") == "");
    CHECK(traced("(s.p.color.x as int) % 8")
          == unmarked("(s.p.color.x as int) % 8",
                      "user:[trace(tables.world, ray(origin = pos3(0.0, 0.0, 0.0), direction = vec3(0.0, 0.0, 1.0)), "
                      "path_rays.surface, mut s.p)] this value is what the shaders the call ran wrote back"));
}

TEST("sgl check - a ray query's hit differs where its ray does, or where the invocations decided its candidates apart")
{
    auto const queried = [](cc::string_view traced, cc::string_view index)
    {
        return reports_for(cc::format("require ray_query\n"
                                      "require binding_arrays\n\n"
                                      "binding scene:\n"
                                      "    world: acceleration_structure[.triangles]\n"
                                      "    results: mut buffer[uint]\n"
                                      "    tables: bytes[8]\n"
                                      "    origin_x: float\n\n"
                                      "@compute(8, 8) fun cs(@thread_id id: int3){{scene}}:\n"
                                      "    let h = {}\n"
                                      "    scene.results[id.x] = scene.tables[{}].load(0u)\n",
                                      traced, index));
    };
    auto const divergent
        = "scene.world.trace(ray(origin = pos3(id.x as float, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)))";
    auto const uniform
        = "scene.world.trace(ray(origin = pos3(scene.origin_x, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0))";

    // CHK-283: every invocation traced a ray of its own
    CHECK(queried(divergent, "nonuniform (h.instance_id % 8)") == "");
    CHECK(queried(divergent, "h.instance_id % 8")
          == unmarked("h.instance_id % 8", "user:[id] this value comes from the stage input id"));

    auto const deciding = [&](cc::string_view accepts)
    { return cc::format("{}, c => if {} => hit_decision.accept else hit_decision.ignore)", uniform, accepts); };

    // one ray, decided alike everywhere, hits the same instance in every invocation
    CHECK(queried(cc::format("{})", uniform), "h.instance_id % 8") == "");
    CHECK(queried(cc::format("{})", uniform), "nonuniform (h.instance_id % 8)").contains("needless-nonuniform"));
    CHECK(queried(deciding("scene.origin_x > 0.0"), "h.instance_id % 8") == "");

    // but where the invocations decide its candidates apart, what the query committed differs between them
    CHECK(queried(deciding("id.x > 3"), "nonuniform (h.instance_id % 8)") == "");
    CHECK(queried(deciding("id.x > 3"), "h.instance_id % 8").contains("non-uniform-index"));
}

TEST("sgl check - a note on a local set under a branch of the prelude names the value that branch tested")
{
    auto const queried = [](cc::string_view body)
    {
        return reports_for(cc::format("require ray_query\n"
                                      "require binding_arrays\n\n"
                                      "binding scene:\n"
                                      "    world: acceleration_structure[.triangles]\n"
                                      "    results: mut buffer[uint]\n"
                                      "    tables: bytes[8]\n"
                                      "    origin_x: float\n\n"
                                      "@compute(8, 8) fun cs(@thread_id id: int3){{scene}}:\n"
                                      "{}",
                                      body));
    };
    auto const traced = "scene.world.trace(ray(origin = pos3(scene.origin_x, 0.0, -1.0), direction = vec3(0.0, 0.0, "
                        "1.0)), c => if id.x > 3 => hit_decision.accept else hit_decision.ignore)";
    auto const decided
        = "user:[if id.x > 3 => hit_decision.accept else hit_decision.ignore] this value is set where the control flow "
          "differs";

    // the query commits under the prelude's branch on the decision, which no line of the program shows
    CHECK(queried(cc::format("    let h = {}\n"
                             "    scene.results[id.x] = scene.tables[h.instance_id % 8].load(0u)\n",
                             traced))
          == unmarked("h.instance_id % 8", decided));

    // CHK-284: every invocation is back after the query's loop, so a break on a uniform condition leaves them together
    CHECK(queried(cc::format("    let mut k = 0\n"
                             "    for i in 0 ..< 4:\n"
                             "        let h = {}\n"
                             "        if scene.origin_x > 0.0 => break\n"
                             "    k = 1\n"
                             "    scene.results[id.x] = scene.tables[k % 8].load(0u)\n",
                             traced))
          == "");
}

TEST("sgl check - every parameter of a stage differs between its invocations, a tessellation's factors among them")
{
    // CHK-283: the invocations of one wave evaluate the domains of several patches
    auto const evaluated = [](cc::string_view index)
    {
        return reports_for(cc::format("require tessellation_shader\n"
                                      "require binding_arrays\n\n"
                                      "struct control_point:\n"
                                      "    position: float2\n\n"
                                      "struct staged:\n"
                                      "    @position position: hpos4\n\n"
                                      "struct tri_factors:\n"
                                      "    @edge_factors edges: float[3]\n"
                                      "    @inside_factors inside: float\n\n"
                                      "binding tables:\n"
                                      "    buffers: bytes[8]\n\n"
                                      "@tessellation_evaluation\n"
                                      "fun domain_te(patch: control_point[3], f: tri_factors, @domain_location uvw: "
                                      "float3){{tables}} -> staged:\n"
                                      "    let k = tables.buffers[{}].load(0u) as float\n"
                                      "    return {{ position = hpos4(k, 0.0, 0.5, 1.0) }}\n",
                                      index));
    };
    CHECK(evaluated("nonuniform ((f.inside as int) % 8)") == "");
    CHECK(evaluated("(f.inside as int) % 8")
          == unmarked("(f.inside as int) % 8", "user:[f] this value comes from the stage's parameter f"));
}

TEST("sgl check - a loop left only by break or continue reconverges after it, for a derivative and a subgroup "
     "operation")
{
    // CHK-284: every pixel that entered the loop reaches the sample after it
    CHECK(reports_for(pixel("    for i in 0 ..< 8:\n"
                            "        if p.uv.x < 0.5 => break\n"
                            "        c.x += 1.0\n"
                            "    c = material.albedo.sample(p.uv)\n"))
          == "");
    CHECK(reports_for(pixel("    while c.x < p.uv.x:\n"
                            "        c.x += 0.25\n"
                            "        if p.uv.y < 0.5 => continue\n"
                            "        c.y += 1.0\n"
                            "    c = material.albedo.sample(p.uv)\n"))
          == "");
    // inside it, the pixels that left miss the later iterations
    CHECK(reports_for(pixel("    for i in 0 ..< 8:\n"
                            "        c = material.albedo.sample(p.uv)\n"
                            "        if p.uv.x < 0.5 => break\n"))
              .contains(kind));
    // a pixel that returns from inside the loop never reaches what follows it
    auto const returned = reports_for(pixel("    for i in 0 ..< 8:\n"
                                            "        if p.uv.x < 0.5 => return {color = c}\n"
                                            "    c = material.albedo.sample(p.uv)\n"));
    CHECK(returned.contains(kind));
    CHECK(returned.contains("some invocations return here, and the others go on"));

    // a helper's early return leaves the rest of its block divergent, and its caller's flow as it was
    auto const helper = cc::string("\nfun first(x: float) -> float:\n"
                                   "    if x < 0.5 => return 0.0\n"
                                   "    return 1.0\n");
    CHECK(reports_for(pixel("    c.x = first(p.uv.x)\n    c = material.albedo.sample(p.uv)\n") + helper) == "");
    auto const sampled_late = cc::string("\nfun late(x: float){material} -> float4:\n"
                                         "    if x < 0.5 => return float4(0.0, 0.0, 0.0, 1.0)\n"
                                         "    return material.albedo.sample(float2(x, x))\n");
    CHECK(reports_for(pixel("    c = late(p.uv.x)\n") + sampled_late).contains(kind));
    // and what stands ahead of its early return is reached by every pixel, since a block runs once
    auto const sampled_early = cc::string("\nfun early(x: float){material} -> float4:\n"
                                          "    let s = material.albedo.sample(float2(x, x))\n"
                                          "    if x < 0.5 => return s\n"
                                          "    return s * 2.0\n");
    CHECK(reports_for(pixel("    c = early(p.uv.x)\n") + sampled_early) == "");
}

TEST("sgl check - a note on a branch of the prelude names the program's call that reached it")
{
    auto const queried = [](cc::string_view body)
    {
        return reports_for(cc::format("require ray_query\n\n"
                                      "binding scene:\n"
                                      "    world: acceleration_structure[.triangles]\n"
                                      "    results: mut buffer[uint]\n\n"
                                      "@compute(8, 8) fun cs(@thread_id id: int3){{scene}}:\n"
                                      "{}",
                                      body));
    };
    // a barrier keeps WGSL's rules, by which the flow after the query's loop differs, and the query is the line to blame
    auto const traced
        = "scene.world.trace(ray(origin = pos3(id.x as float, 0.0, -1.0), direction = vec3(0.0, 0.0, 1.0)))";
    auto const barred = queried(cc::format("    let h = {}\n"
                                           "    workgroup_barrier()\n"
                                           "    scene.results[id.x] = h.instance_id as uint\n",
                                           traced));
    CHECK(barred
          == cc::format("non-uniform-control-flow user:[workgroup_barrier()] workgroup_barrier waits for every thread "
                        "of "
                        "the workgroup, and not every one reaches it here\n"
                        "  note user:[{}] a branch inside this call tests a value that comes from the stage input id\n",
                        traced));
}
