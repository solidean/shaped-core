#include <nexus/test.hh>
#include <shader-analysis/op_kind.hh>

using namespace cc::primitive_defines;

TEST("shan op_kind - SPIR-V opcodes fall into what a GPU spends on them")
{
    CHECK(shan::spirv_kind_of(129) == shan::op_kind::float_arithmetic); // OpFAdd
    CHECK(shan::spirv_kind_of(148) == shan::op_kind::float_arithmetic); // OpDot
    CHECK(shan::spirv_kind_of(128) == shan::op_kind::int_arithmetic);   // OpIAdd
    CHECK(shan::spirv_kind_of(196) == shan::op_kind::int_arithmetic);   // OpShiftLeftLogical
    CHECK(shan::spirv_kind_of(12) == shan::op_kind::extended);          // OpExtInst
    CHECK(shan::spirv_kind_of(61) == shan::op_kind::load);
    CHECK(shan::spirv_kind_of(62) == shan::op_kind::store);
    CHECK(shan::spirv_kind_of(65) == shan::op_kind::address);   // OpAccessChain
    CHECK(shan::spirv_kind_of(81) == shan::op_kind::composite); // OpCompositeExtract
    CHECK(shan::spirv_kind_of(169) == shan::op_kind::logic);    // OpSelect
    CHECK(shan::spirv_kind_of(184) == shan::op_kind::compare);  // OpFOrdLessThan
    CHECK(shan::spirv_kind_of(87) == shan::op_kind::image);     // OpImageSampleImplicitLod
    CHECK(shan::spirv_kind_of(234) == shan::op_kind::atomic);   // OpAtomicIAdd
    CHECK(shan::spirv_kind_of(245) == shan::op_kind::phi);
    CHECK(shan::spirv_kind_of(250) == shan::op_kind::control);    // OpBranchConditional
    CHECK(shan::spirv_kind_of(71) == shan::op_kind::declaration); // OpDecorate
    CHECK(shan::spirv_kind_of(43) == shan::op_kind::declaration); // OpConstant
    CHECK(shan::name_of(shan::op_kind::float_arithmetic) == "float_arithmetic");
    CHECK(shan::latency_of(shan::op_kind::load) > shan::latency_of(shan::op_kind::float_arithmetic));
}
