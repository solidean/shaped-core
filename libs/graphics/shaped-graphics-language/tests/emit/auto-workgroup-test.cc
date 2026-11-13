#include "../check/check-test-support.hh"
#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::target;

namespace
{
constexpr cc::string_view k_work = "binding work:\n"
                                   "    values: mut buffer[float]\n"
                                   "\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}

/// A bare @compute over `body`, which reads the thread id `id`.
cc::string bare(cc::string_view body)
{
    return cc::format("{}@compute fun cs(@thread_id id: int3){{work}}:\n{}", k_work, body);
}
} // namespace

TEST("sgl emit - a bare @compute's size follows the axes of the thread id its body reads (CHK-520)")
{
    CHECK(text_of(bare("    work.values[id.x] = 1.0\n"), target::hlsl_dx12).contains("[numthreads(64, 1, 1)]"));
    CHECK(text_of(bare("    work.values[id.x + id.y * 64] = 1.0\n"), target::hlsl_dx12).contains("[numthreads(8, 8, 1)]"));
    // a swizzle reads its highest letter's axis, and the vector handed on whole reads every one
    CHECK(text_of(bare("    let p = id.yx\n    work.values[p.x] = 1.0\n"), target::wgsl)
              .contains("@compute @workgroup_size(8, 8, 1)"));
    CHECK(text_of(bare("    work.values[id.z] = 1.0\n"), target::hlsl_vulkan).contains("[numthreads(4, 4, 4)]"));
    CHECK(
        text_of(bare("    let p = id\n    work.values[p.x] = 1.0\n"), target::hlsl_dx12).contains("[numthreads(4, 4, 4)]"));
    // through a call too, since the tree is the body with every call inlined
    auto const called = cc::format("{}fun at(p: int3) -> int => p.x + p.y * 8\n\n"
                                   "@compute fun cs(@thread_id id: int3){{work}}:\n"
                                   "    work.values[at(id)] = 1.0\n",
                                   k_work);
    CHECK(text_of(called, target::hlsl_dx12).contains("[numthreads(4, 4, 4)]"));
    // a body that reads no thread id runs as wide a row as one that reads x
    CHECK(text_of(cc::format("{}@compute fun cs(){{work}}:\n    work.values[0] = 1.0\n", k_work), target::hlsl_dx12)
              .contains("[numthreads(64, 1, 1)]"));
}

TEST("sgl emit - a bare @compute's threads past the grid sg writes return first, and metal's dispatch has none "
     "(CHK-520)")
{
    auto const source = bare("    work.values[id.x] = 1.0\n");
    auto const dx12 = text_of(source, target::hlsl_dx12);
    CHECK(dx12.contains("ConstantBuffer<sg_dispatch_grid_data> sg_dispatch_grid : register(b2, space12);")).dump("text", dx12);
    CHECK(dx12.contains("sg_dispatch_grid.threads.x")).dump("text", dx12);
    CHECK(dx12.contains("sg_dispatch_grid.threads.z")).dump("text", dx12);
    // the guard stands before the body
    CHECK(dx12.find("sg_dispatch_grid.threads.x") < dx12.find("work_values[id.x] = 1.0")).dump("text", dx12);

    auto const vulkan = text_of(source, target::hlsl_vulkan);
    CHECK(vulkan.contains("[[vk::binding(21, 3)]] ConstantBuffer<sg_dispatch_grid_data> sg_dispatch_grid;"))
        .dump("text", vulkan);
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("@group(3) @binding(21) var<uniform> sg_dispatch_grid: sg_dispatch_grid_data;")).dump("text", wgsl);
    auto const msl = text_of(source, target::msl);
    CHECK(!msl.contains("sg_dispatch_grid")).dump("text", msl);

    // a stated size is the author's, whose grid is the author's to test
    auto const stated
        = cc::format("{}@compute(64) fun cs(@thread_id id: int3){{work}}:\n    work.values[id.x] = 1.0\n", k_work);
    CHECK(!text_of(stated, target::hlsl_dx12).contains("sg_dispatch_grid"));

    // a body that takes no thread id is given one to test
    auto const untaken
        = text_of(cc::format("{}@compute fun cs(){{work}}:\n    work.values[0] = 1.0\n", k_work), target::hlsl_dx12);
    CHECK(untaken.contains("SV_DispatchThreadID")).dump("text", untaken);
    CHECK(untaken.contains("sg_dispatch_grid.threads.x")).dump("text", untaken);
}

TEST("sgl driver - a bare @compute is dispatched by threads alone, which its compile says (CHK-520)")
{
    for (auto const t : {target::hlsl_dx12, target::msl})
    {
        auto const r
            = sgl::compile_to_text({.source = bare("    work.values[id.x] = 1.0\n"), .entry_point = "cs", .target = t});
        REQUIRE(r.has_value()).context(r.has_error() ? r.error() : cc::string());
        CHECK(r.value().is_dispatched_by_threads);
        CHECK(r.value().workgroup[0] == 64);
    }
    auto const stated = sgl::compile_to_text({.source = cc::format("{}@compute(64) fun cs(@thread_id id: "
                                                                   "int3){{work}}:\n    work.values[id.x] = 1.0\n",
                                                                   k_work),
                                              .entry_point = "cs",
                                              .target = target::hlsl_dx12});
    REQUIRE(stated.has_value());
    CHECK(!stated.value().is_dispatched_by_threads);
}

TEST("sgl check - a bare @compute reads nothing of the workgroup it chooses (CHK-520)")
{
    auto const refused = "a bare @compute chooses its own workgroup size";
    CHECK(reports_for(bare("    work.values[id.x] = 1.0\n")) == "");
    // a barrier and a subgroup operation, here and in what the body calls
    CHECK(reports_for(bare("    workgroup_barrier()\n    work.values[id.x] = 1.0\n"))
              .contains("reads the workgroup it runs in"));
    CHECK(reports_for(cc::format("require subgroups\n\nfun total(v: float) -> float => subgroup_add(v)\n\n{}",
                                 bare("    work.values[id.x] = total(1.0)\n")))
              .contains("subgroup_add reads the workgroup it runs in"));
    // workgroup memory, and an input that says where an invocation stands in its group
    CHECK(reports_for(cc::format("@workgroup binding tile:\n    cells: float[64]\n\n{}"
                                 "@compute fun cs(@thread_id id: int3){{work, tile}}:\n"
                                 "    work.values[id.x] = 1.0\n",
                                 k_work))
              .contains(refused));
    CHECK(reports_for(cc::format("{}@compute fun cs(@local_thread_index li: int){{work}}:\n    work.values[li] = 1.0\n",
                                 k_work))
              .contains(refused));
    CHECK(reports_for(cc::format("{}@compute fun cs(@workgroup_id g: int3){{work}}:\n    work.values[g.x] = 1.0\n", k_work))
              .contains(refused));
}

TEST("sgl check - a stated size that idles lanes, that webgpu refuses, or that the indexing contradicts warns "
     "(CHK-521)")
{
    auto const stated = [](cc::string_view size, cc::string_view body)
    { return reports_for(cc::format("{}@compute({}) fun cs(@thread_id id: int3){{work}}:\n{}", k_work, size, body)); };
    auto const x = cc::string_view("    work.values[id.x] = 1.0\n");
    auto const xy = cc::string_view("    work.values[id.x + id.y * 64] = 1.0\n");
    CHECK(stated("16", x).contains("workgroup-wastes-lanes"));
    CHECK(stated("16", x).contains("less than a wave"));
    CHECK(stated("48", x).contains("a partial wave in every workgroup"));
    CHECK(stated("512", x).contains("which no webgpu device runs"));
    CHECK(stated("32, 1, 128", "    work.values[id.x + id.z] = 1.0\n").contains("which no webgpu device runs"));
    CHECK(stated("8, 8", x).contains("every row of a workgroup but the first runs past it"));
    CHECK(stated("8, 8", x).contains("a bare @compute would choose (64) here"));

    // a serial kernel, a size that fits the indexing, and a row over a 2D grid are no waste
    CHECK(stated("1", x) == "");
    CHECK(stated("64", x) == "");
    CHECK(stated("8, 8", xy) == "");
    CHECK(stated("64", xy) == "");
    CHECK(stated("256", xy) == "");
    // a body that reads no thread id says nothing of its shape
    CHECK(stated("8, 8", "    work.values[0] = 1.0\n") == "");

    // a group a barrier, workgroup memory or a group input reads is sized by its algorithm, which no warning judges
    CHECK(stated("4", "    workgroup_barrier()\n    work.values[id.x] = 1.0\n") == "");
    CHECK(reports_for(cc::format(
              "{}@compute(4) fun cs(@local_thread_index li: int){{work}}:\n    work.values[li] = 1.0\n", k_work))
          == "");
}
