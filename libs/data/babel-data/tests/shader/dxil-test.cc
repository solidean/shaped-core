#include <babel-data/shader/dxil_container.hh>
#include <babel-data/shader/llvm37.hh>
#include <babel-data/shader/llvm_bitstream.hh>
#include <clean-core/streams/file_stream.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// tiny_cs.dxil is DXC 1.9.2602's `-T cs_6_6 -O3 -Qstrip_reflect -Qstrip_debug` of this, a loop, a branch and a sqrt:
//
//   StructuredBuffer<float4> source : register(t0);
//   RWStructuredBuffer<float> target : register(u0);
//   [numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
//   {
//       float4 v = source[id.x];
//       float total = 0.0;
//       for (uint i = 0; i < id.y; ++i)
//           total += v.x * float(i) + v.y;
//       target[id.x] = id.x > 3 ? total : sqrt(v.z);
//   }

namespace
{
[[nodiscard]] cc::vector<byte> tiny_cs()
{
    auto file = cc::file_read_stream_adapter::open(nx::source_path(BABEL_DATA_TEST_DIR "/shader/data/tiny_cs.dxil"));
    REQUIRE(file.has_value());
    auto stream = file.value().stream();
    auto bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    return cc::move(bytes.value());
}
} // namespace

TEST("babel dxil - a container reads as its parts, and its program as its shader kind, model and bitcode")
{
    auto const c = babel::dxil::read(tiny_cs());
    REQUIRE(c.has_value());
    REQUIRE(c.value().part_named("DXIL") != nullptr);
    CHECK(c.value().part_named("PSV0") != nullptr);
    CHECK(c.value().part_named("NOPE") == nullptr);
    auto const p = babel::dxil::program_of(*c.value().part_named("DXIL"));
    REQUIRE(p.has_value());
    CHECK(p.value().shader_kind == 5);
    CHECK(p.value().model_major == 6);
    CHECK(p.value().model_minor == 6);
    CHECK(p.value().bitcode.size() > 0);

    auto broken = tiny_cs();
    broken[0] = byte('X');
    CHECK(babel::dxil::read(broken).has_error());
}

TEST("babel llvm37 - DXC's DXIL reads as its functions, their blocks and instructions, each operand a value id")
{
    auto const c = babel::dxil::read(tiny_cs()).value();
    auto const p = babel::dxil::program_of(*c.part_named("DXIL")).value();
    auto const bc = babel::llvm::read_bitstream(p.bitcode);
    REQUIRE(bc.has_value());
    auto const m = babel::llvm37::read(bc.value());
    REQUIRE(m.has_value()).dump("error", m.has_error() ? m.error().to_string() : cc::string());
    auto const& mod = m.value();
    CHECK(mod.triple == "dxil-ms-dx");
    REQUIRE(mod.bodies.size() == 1);
    auto const& fn = mod.functions[mod.bodies[0].function];
    CHECK(fn.name == "main");
    auto const& b = mod.bodies[0];
    CHECK(b.block_count == 7);
    CHECK(b.instructions.size() == 31);

    auto phis = 0;
    auto names = cc::vector<cc::string>();
    for (auto const& i : b.instructions)
    {
        phis += i.code == babel::llvm37::inst_code::phi ? 1 : 0;
        if (i.code == babel::llvm37::inst_code::call)
            names.push_back(cc::string(mod.callee_name(b, i)));
        // every value an instruction reads is one the module or the body holds
        auto const operands = mod.operands_of(b, i);
        for (auto k = isize(0); k < operands.size(); ++k)
            if (b.roles[i.first + k] == babel::llvm37::operand_role::value)
                CHECK(mod.value_of(b, operands[k]).index != 0xffffffffu);
    }
    CHECK(phis == 4);
    auto const calls = [&](cc::string_view name)
    {
        for (auto const& n : names)
            if (n == name)
                return true;
        return false;
    };
    CHECK(calls("dx.op.rawBufferLoad.f32"));
    CHECK(calls("dx.op.unary.f32"));

    // the loop's phi reads the add its own block ends with, an operand that refers forward
    auto found = false;
    for (auto const& i : b.instructions)
    {
        if (i.code != babel::llvm37::inst_code::phi || i.block != 4)
            continue;
        for (auto const v : mod.operands_of(b, i))
        {
            auto const target = mod.value_of(b, v);
            if (target.kind == babel::llvm37::value_kind::instruction && target.index < u32(b.instructions.size()))
                found = found || b.instructions[target.index].code == babel::llvm37::inst_code::binop;
        }
    }
    CHECK(found);
}

TEST("babel llvm bitstream - bytes without the bitcode magic are an error")
{
    auto const junk = cc::vector<byte>::create_filled(64, byte(7));
    CHECK(babel::llvm::read_bitstream(junk).has_error());
}
