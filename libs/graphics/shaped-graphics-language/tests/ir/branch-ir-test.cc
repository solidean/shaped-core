#include <babel-data/shader/dxil_container.hh>
#include <babel-data/shader/llvm37.hh>
#include <babel-data/shader/llvm_bitstream.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
#include <shaped-graphics-language/ir/dxil/impl/llvm.hh>

using namespace cc::primitive_defines;

namespace
{
/// The `xor`s in the DXIL SGL writes of a compute stage that leaves where `condition` holds and stores otherwise.
[[nodiscard]] int xors_leaving_where(cc::string_view condition)
{
    auto const source = cc::format("binding io:\n"
                                   "    values: mut buffer[int]\n"
                                   "\n"
                                   "@compute(64) fun cs(@thread_id id: int3){{io}}:\n"
                                   "    let v = io.values[id.x]\n"
                                   "    if {}:\n"
                                   "        return\n"
                                   "    io.values[id.x] = v * 2\n",
                                   condition);
    auto const e = sgl::compile_to_text({.source = source,
                                         .entry_point = "cs",
                                         .target = sgl::emit::target::hlsl_dx12,
                                         .native = sgl::emit::native_route::require});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const c = babel::dxil::read(e.value().dxil);
    REQUIRE(c.has_value());
    auto const* const part = c.value().part_named("DXIL");
    REQUIRE(part != nullptr);
    auto const p = babel::dxil::program_of(*part);
    REQUIRE(p.has_value());
    auto const bc = babel::llvm::read_bitstream(p.value().bitcode);
    REQUIRE(bc.has_value());
    auto const m = babel::llvm37::read(bc.value());
    REQUIRE(m.has_value());
    auto xors = 0;
    for (auto const& f : m.value().functions)
        if (f.body >= 0)
            for (auto const& i : m.value().bodies[f.body].instructions)
                if (i.code == babel::llvm37::inst_code::binop && i.sub == u32(sgl::ir::dxil::impl::binary_op::bit_xor))
                    ++xors;
    return xors;
}
} // namespace

TEST("sgl dxil - a branch on a negation branches on what it negates, its ways swapped (EMIT-285)")
{
    // AMD's D3D12 driver takes `v` as 3 past a branch on `xor (icmp ne v, 3), true`, so no such xor may reach a branch
    CHECK(xors_leaving_where("not (v != 3)") == xors_leaving_where("v == 3"));
    CHECK(xors_leaving_where("not (v < 3)") == xors_leaving_where("v >= 3"));
}
