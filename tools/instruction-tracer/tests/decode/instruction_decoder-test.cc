#include <clean-core/container/vector.hh>
#include <instruction-tracer/decode/instruction_decoder.hh>
#include <nexus/test.hh>

using namespace itrace;

namespace
{
/// A record holding `bytes`, as if the tracer had captured them at `rip`.
recorded_instruction at(u64 rip, cc::span<u8 const> bytes)
{
    recorded_instruction insn;
    insn.rip = rip;
    insn.byte_count = u8(bytes.size());
    for (isize i = 0; i < bytes.size(); ++i)
        insn.bytes[i] = bytes[i];
    return insn;
}

recorded_instruction decoded(cc::span<u8 const> bytes, u64 rip = 0x140001000)
{
    instruction_decoder const decoder;
    auto insn = at(rip, bytes);
    decoder.decode_one(insn);
    return insn;
}
} // namespace

TEST("decoder - ret")
{
    auto const insn = decoded({0xC3});
    CHECK(insn.length == 1);
    CHECK(insn.category == insn_category::ret);
    CHECK(insn.text == "ret");
}

TEST("decoder - mov rbp, rsp")
{
    auto const insn = decoded({0x48, 0x89, 0xE5});
    CHECK(insn.length == 3);
    CHECK(insn.category == insn_category::other);
    CHECK(insn.text.contains("mov"));
    CHECK(insn.text.contains("rbp"));
    CHECK(insn.text.contains("rsp"));
}

TEST("decoder - conditional branch")
{
    // 74 16 = jz +0x16
    auto const insn = decoded({0x74, 0x16});
    CHECK(insn.length == 2);
    CHECK(insn.category == insn_category::conditional_branch);
}

TEST("decoder - unconditional branch")
{
    // EB 10 = jmp +0x10
    auto const insn = decoded({0xEB, 0x10});
    CHECK(insn.length == 2);
    CHECK(insn.category == insn_category::unconditional_branch);
}

TEST("decoder - direct and indirect call")
{
    // E8 rel32
    auto const direct = decoded({0xE8, 0x00, 0x00, 0x00, 0x00});
    CHECK(direct.length == 5);
    CHECK(direct.category == insn_category::call);

    // FF D0 = call rax
    auto const indirect = decoded({0xFF, 0xD0});
    CHECK(indirect.length == 2);
    CHECK(indirect.category == insn_category::call);
    CHECK(indirect.text.contains("rax"));
}

TEST("decoder - syscall")
{
    auto const insn = decoded({0x0F, 0x05});
    CHECK(insn.length == 2);
    CHECK(insn.category == insn_category::syscall);
    CHECK(insn.text.contains("syscall"));
}

TEST("decoder - int3 is not a syscall")
{
    // Zydis files int3 under INTERRUPT alongside `int 0x2e`; only the kernel gate should count,
    // otherwise a breakpoint byte would end every trace.
    auto const insn = decoded({0xCC});
    CHECK(insn.length == 1);
    CHECK(insn.category != insn_category::syscall);
}

TEST("decoder - decodes only the first instruction of the byte window")
{
    // The tracer always records a full 15-byte window; the decoder must stop after the first.
    auto const insn = decoded({0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90});
    CHECK(insn.length == 1);
    CHECK(insn.category == insn_category::ret);
}

TEST("decoder - rip only affects rip-relative rendering")
{
    // 48 8B 05 disp32 = mov rax, [rip + disp32]; the printed target folds in rip.
    cc::vector<u8> const bytes = {0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00};

    auto const a = decoded(bytes, 0x140001000);
    auto const b = decoded(bytes, 0x140002000);

    CHECK(a.length == 7);
    CHECK(b.length == 7);
    CHECK(a.text != b.text);
}

TEST("decoder - undecodable bytes leave the record empty")
{
    // Not a valid encoding; must fail softly rather than assert.
    auto const insn = decoded({0xFF, 0xFF, 0xFF, 0xFF});
    CHECK(insn.length == 0);
    CHECK(insn.text.empty());
}

TEST("decoder - empty record is a no-op")
{
    instruction_decoder const decoder;
    recorded_instruction insn;
    decoder.decode_one(insn);
    CHECK(insn.length == 0);
}

TEST("decoder - decode over a span fills every record")
{
    instruction_decoder const decoder;

    cc::vector<recorded_instruction> insns;
    insns.push_back(at(0x1000, {0xC3}));
    insns.push_back(at(0x1001, {0x90}));

    decoder.decode(insns);

    CHECK(insns[0].category == insn_category::ret);
    CHECK(insns[1].length == 1);
    CHECK(insns[1].text.contains("nop"));
}

TEST("is_syscall_bytes - matches the kernel gates only")
{
    CHECK(is_syscall_bytes(cc::vector<u8>{0x0F, 0x05})); // syscall
    CHECK(is_syscall_bytes(cc::vector<u8>{0x0F, 0x34})); // sysenter
    CHECK(is_syscall_bytes(cc::vector<u8>{0xCD, 0x2E})); // int 0x2e

    CHECK(!is_syscall_bytes(cc::vector<u8>{0xCC}));       // int3
    CHECK(!is_syscall_bytes(cc::vector<u8>{0xCD, 0x03})); // int 3, a different vector
    CHECK(!is_syscall_bytes(cc::vector<u8>{0xC3}));       // ret
    CHECK(!is_syscall_bytes(cc::vector<u8>{0x0F}));       // truncated
    CHECK(!is_syscall_bytes(cc::span<u8 const>()));       // empty
}
