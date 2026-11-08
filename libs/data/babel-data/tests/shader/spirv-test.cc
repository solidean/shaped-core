#include <babel-data/shader/spirv.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr u32 word(u16 opcode, u16 count)
{
    return (u32(count) << 16) | opcode;
}

/// A compute entry point `main` that returns at once, assembled by hand.
cc::vector<u32> tiny_module()
{
    // "main" is 4 bytes, so its terminating zero takes a word of its own
    auto const main0 = u32('m') | (u32('a') << 8) | (u32('i') << 16) | (u32('n') << 24);
    return cc::vector<u32>{
        0x07230203u,  0x00010600u, 0, 5,     0, // header: SPIR-V 1.6, bound 5
        word(17, 2),  1,                        // OpCapability Shader
        word(14, 3),  0,           1,           // OpMemoryModel Logical GLSL450
        word(15, 5),  5,           4, main0, 0, // OpEntryPoint GLCompute %4 "main"
        word(19, 2),  1,                        // %1 = OpTypeVoid
        word(33, 3),  2,           1,           // %2 = OpTypeFunction %1
        word(54, 5),  1,           4, 0,     2, // %4 = OpFunction %1 None %2
        word(248, 2), 3,                        // %3 = OpLabel
        word(253, 1),                           // OpReturn
        word(56, 1),                            // OpFunctionEnd
    };
}
} // namespace

TEST("babel spirv - a module reads as its header and its instructions, each knowing its result")
{
    auto const words = tiny_module();
    auto const m = babel::spirv::read(cc::span<u32 const>(words));
    REQUIRE(m.has_value());
    auto const& mod = m.value();
    CHECK(mod.major_version() == 1);
    CHECK(mod.minor_version() == 6);
    CHECK(mod.head.bound == 5);
    REQUIRE(mod.instruction_count() == 9);

    CHECK(mod.at(0).opcode() == 17);
    CHECK(mod.at(0).result_id() == 0);
    auto const entry = mod.at(2);
    auto taken = isize(0);
    CHECK(entry.string_at(2, &taken) == "main");
    CHECK(taken == 2);

    auto const function = mod.at(5);
    CHECK(function.result_type() == 1);
    CHECK(function.result_id() == 4);
    CHECK(function.arguments().size() == 2);
    CHECK(mod.at(6).result_id() == 3);
    CHECK(mod.at(6).result_type() == 0);

    REQUIRE(babel::spirv::info_of(129) != nullptr);
    CHECK(cc::string_view(babel::spirv::info_of(129)->name) == "OpFAdd");
    CHECK(babel::spirv::info_of(0xfffe) == nullptr);
}

TEST("babel spirv - a module written big-endian reads as the same words")
{
    auto const words = tiny_module();
    auto bytes = cc::vector<byte>();
    for (auto const w : words)
        for (auto k = 3; k >= 0; --k)
            bytes.push_back(byte((w >> (8 * k)) & 0xffu));
    auto const m = babel::spirv::read(cc::span<byte const>(bytes));
    REQUIRE(m.has_value());
    CHECK(m.value().head.magic == 0x07230203u);
    REQUIRE(m.value().instruction_count() == 9);
    CHECK(m.value().at(5).result_id() == 4);
}

TEST("babel spirv - a module short of its header, its words or its magic is an error, never a partial read")
{
    auto const words = tiny_module();
    auto const all = cc::span<u32 const>(words);
    CHECK(babel::spirv::read(all.subspan({.offset = 0, .size = 4})).has_error());

    // the last instruction cut short, and one counting zero words
    CHECK(babel::spirv::read(all.subspan({.offset = 0, .size = all.size() - 1})).has_value());
    auto cut = cc::vector<u32>::create_copy_of(all);
    cut[isize(cut.size()) - 1] = word(56, 3);
    CHECK(babel::spirv::read(cc::span<u32 const>(cut)).has_error());
    cut[isize(cut.size()) - 1] = word(56, 0);
    CHECK(babel::spirv::read(cc::span<u32 const>(cut)).has_error());

    auto bad = cc::vector<u32>::create_copy_of(all);
    bad[0] = 0x12345678u;
    CHECK(babel::spirv::read(cc::span<u32 const>(bad)).has_error());

    auto odd = cc::vector<byte>::create_filled(22, byte(0));
    CHECK(babel::spirv::read(cc::span<byte const>(odd)).has_error());
}
