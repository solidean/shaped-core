#include "pipeline.hh"

#include <clean-core/common/asserts.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ir/dxil/dxil.hh>
#include <shaped-graphics-language/ir/lower.hh>
#include <shaped-graphics-language/ir/passes.hh>
#include <shaped-graphics-language/ir/simplify.hh>
#include <shaped-graphics-language/ir/spirv/spirv.hh>
#include <shaped-graphics-language/ir/ssa.hh>
#include <shaped-graphics-language/ir/verify.hh>

using namespace sgl;
using namespace sgl::ir;

namespace
{
/// A pass's mistake stops here, where assertions are on, rather than reaching a driver.
void assert_whole([[maybe_unused]] program const& p,
                  [[maybe_unused]] control_flow flow,
                  [[maybe_unused]] cc::string_view after)
{
#if CC_ASSERT_ENABLED
    auto const wrong = verify(p, flow);
    CC_ASSERTS(!wrong.has_value(), cc::format("the program after {} of '{}': {}", after, p.entry_name,
                                              wrong.has_value() ? wrong.value() : ""));
#endif
}

/// Lowering and SSA, which every target shares.
[[nodiscard]] cc::optional<cc::string> lower_to_ssa(emit::impl::plan const& plan, program& p)
{
    auto gap = lower(plan, p);
    if (gap.has_value())
        return gap;
    assert_whole(p, control_flow::structured, "lowering");
    construct_ssa(p);
    assert_whole(p, control_flow::structured, "construct_ssa");
    state_constant_tables(p);
    assert_whole(p, control_flow::structured, "state_constant_tables");
    return {};
}
} // namespace

cc::optional<cc::string> sgl::ir::compile_spirv(emit::impl::plan const& plan, program& p, cc::vector<u32>& out)
{
    if (auto gap = lower_to_ssa(plan, p); gap.has_value())
        return gap;
    // DXIL's passes, each keeping what a SPIR-V header names: its merge, and a loop's continue target
    simplify(p, control_flow::structured);
    assert_whole(p, control_flow::structured, "simplify");
    regroup_lanes(p);
    assert_whole(p, control_flow::structured, "regroup_lanes");
    simplify(p, control_flow::structured);
    assert_whole(p, control_flow::structured, "simplify after regroup_lanes");
    share_common_values(p);
    assert_whole(p, control_flow::structured, "share_common_values");
    hoist_common_values(p);
    assert_whole(p, control_flow::structured, "hoist_common_values");
    remove_unused(p);
    assert_whole(p, control_flow::structured, "remove_unused");
    flatten_branches(p, control_flow::structured);
    assert_whole(p, control_flow::structured, "flatten_branches");
    chain_sums(p);
    assert_whole(p, control_flow::structured, "chain_sums");
    out = write_spirv(p);
    return {};
}

cc::optional<cc::string> sgl::ir::compile_dxil(emit::impl::plan const& plan, program& p, cc::vector<byte>& out)
{
    if (auto gap = lower_to_ssa(plan, p); gap.has_value())
        return gap;
    simplify(p);
    assert_whole(p, control_flow::unstructured, "simplify");
    share_common_values(p);
    assert_whole(p, control_flow::unstructured, "share_common_values");
    hoist_common_values(p);
    assert_whole(p, control_flow::unstructured, "hoist_common_values");
    remove_unused(p);
    assert_whole(p, control_flow::unstructured, "remove_unused");
    flatten_branches(p);
    assert_whole(p, control_flow::unstructured, "flatten_branches");
    chain_sums(p);
    assert_whole(p, control_flow::unstructured, "chain_sums");
    return write_dxil(p, out);
}
