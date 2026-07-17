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

TEST("decoder - lock-prefixed RMW is atomic")
{
    // F0 FF 00 = lock inc dword ptr [rax]
    auto const insn = decoded({0xF0, 0xFF, 0x00});
    CHECK(insn.is_atomic);
    CHECK(insn.reads_memory);
    CHECK(insn.writes_memory);
}

TEST("decoder - xchg with memory is atomic without a lock prefix")
{
    // 48 87 08 = xchg [rax], rcx. The x86 rule: xchg against memory locks implicitly, so there is no
    // prefix and no ZYDIS_ATTRIB_HAS_LOCK. This is how an atomic store/exchange compiles, so matching
    // only the prefix undercounts exactly the traffic the atomics column exists to find.
    auto const insn = decoded({0x48, 0x87, 0x08});
    CHECK(insn.is_atomic);
}

TEST("decoder - xchg between registers is not atomic")
{
    // 48 87 C8 = xchg rax, rcx — no memory operand, nothing to lock.
    auto const insn = decoded({0x48, 0x87, 0xC8});
    CHECK(!insn.is_atomic);
}

TEST("decoder - a plain register move is neither atomic nor memory traffic")
{
    auto const insn = decoded({0x48, 0x89, 0xC8}); // mov rax, rcx
    CHECK(!insn.is_atomic);
    CHECK(!insn.reads_memory);
    CHECK(!insn.writes_memory);
    CHECK(!insn.is_indirect);
}

TEST("decoder - call directness")
{
    CHECK(!decoded({0xE8, 0x00, 0x00, 0x00, 0x00}).is_indirect); // call rel32
    CHECK(decoded({0xFF, 0xD0}).is_indirect);                    // call rax
    CHECK(decoded({0xFF, 0x51, 0x18}).is_indirect);              // call [rcx+0x18] — a vtable hop
}

TEST("decoder - jmp directness")
{
    CHECK(!decoded({0xEB, 0x10}).is_indirect); // jmp rel8
    CHECK(decoded({0xFF, 0xE0}).is_indirect);  // jmp rax
}

TEST("decoder - a conditional branch is never indirect")
{
    // x86 has no indirect conditional branch; the column would be meaningless noise if it said so.
    CHECK(!decoded({0x74, 0x16}).is_indirect); // jz +0x16
}

TEST("decoder - memory read and write are tracked separately")
{
    auto const read = decoded({0x48, 0x8B, 0x01}); // mov rax, [rcx]
    CHECK(read.reads_memory);
    CHECK(!read.writes_memory);

    auto const write = decoded({0x48, 0x89, 0x01}); // mov [rcx], rax
    CHECK(!write.reads_memory);
    CHECK(write.writes_memory);

    auto const rmw = decoded({0x48, 0x01, 0x01}); // add [rcx], rax
    CHECK(rmw.reads_memory);
    CHECK(rmw.writes_memory);
}

TEST("decoder - implicit stack traffic is not counted as memory access")
{
    // push/pop touch memory only through the implicit rsp operand. Counting those would mark most of
    // a prologue, drowning out the pointer chases the column is for.
    CHECK(!decoded({0x50}).writes_memory); // push rax
    CHECK(!decoded({0x58}).reads_memory);  // pop rax
}

TEST("decoder - undecodable bytes leave every flag false")
{
    auto const insn = decoded({0xFF, 0xFF, 0xFF, 0xFF});
    CHECK(insn.length == 0);
    CHECK(!insn.is_atomic);
    CHECK(!insn.is_indirect);
    CHECK(!insn.reads_memory);
    CHECK(!insn.writes_memory);
    CHECK(insn.slow_mnemonic == nullptr);
}

TEST("decoder - integer divide is flagged slow")
{
    // 48 F7 F9 = idiv rcx. The one that arrives uninvited via `%` on a non-power-of-two.
    auto const insn = decoded({0x48, 0xF7, 0xF9});
    REQUIRE(insn.slow_mnemonic != nullptr);
    CHECK(cc::string_view(insn.slow_mnemonic) == "idiv");

    // 48 F7 F1 = div rcx
    auto const unsigned_div = decoded({0x48, 0xF7, 0xF1});
    REQUIRE(unsigned_div.slow_mnemonic != nullptr);
    CHECK(cc::string_view(unsigned_div.slow_mnemonic) == "div");
}

TEST("decoder - multiply is not slow, only divide is")
{
    // 48 F7 E9 = imul rcx — same F7 group as idiv, ~3 cycles. Flagging it would be noise.
    CHECK(decoded({0x48, 0xF7, 0xE9}).slow_mnemonic == nullptr);
}

TEST("decoder - shifts are not slow (a masked bucket index is the fast path)")
{
    // 48 D3 E1 = shl rcx, cl — what a power-of-two modulus compiles to. Flagging this would make the
    // column meaningless for the exact comparison it exists to serve.
    CHECK(decoded({0x48, 0xD3, 0xE1}).slow_mnemonic == nullptr);
    CHECK(decoded({0x48, 0x21, 0xD1}).slow_mnemonic == nullptr); // and rcx, rdx
}

TEST("decoder - timing and serializing instructions are flagged slow")
{
    auto const tsc = decoded({0x0F, 0x31}); // rdtsc
    REQUIRE(tsc.slow_mnemonic != nullptr);
    CHECK(cc::string_view(tsc.slow_mnemonic) == "rdtsc");

    auto const id = decoded({0x0F, 0xA2}); // cpuid
    REQUIRE(id.slow_mnemonic != nullptr);
    CHECK(cc::string_view(id.slow_mnemonic) == "cpuid");
}

TEST("decoder - fences and the spin hint are flagged slow")
{
    auto const mfence = decoded({0x0F, 0xAE, 0xF0});
    REQUIRE(mfence.slow_mnemonic != nullptr);
    CHECK(cc::string_view(mfence.slow_mnemonic) == "mfence");

    // F3 90 = pause. A contended spinlock is exactly where this shows up.
    auto const pause = decoded({0xF3, 0x90});
    REQUIRE(pause.slow_mnemonic != nullptr);
    CHECK(cc::string_view(pause.slow_mnemonic) == "pause");
}

TEST("decoder - float divide and sqrt are flagged slow")
{
    auto const div = decoded({0xF2, 0x0F, 0x5E, 0xC1}); // divsd xmm0, xmm1
    REQUIRE(div.slow_mnemonic != nullptr);
    CHECK(cc::string_view(div.slow_mnemonic) == "divsd");

    auto const sqrt = decoded({0xF2, 0x0F, 0x51, 0xC1}); // sqrtsd xmm0, xmm1
    REQUIRE(sqrt.slow_mnemonic != nullptr);
    CHECK(cc::string_view(sqrt.slow_mnemonic) == "sqrtsd");

    // Multiply and add are single-digit cycles — not this column's business.
    CHECK(decoded({0xF2, 0x0F, 0x59, 0xC1}).slow_mnemonic == nullptr); // mulsd
    CHECK(decoded({0xF2, 0x0F, 0x58, 0xC1}).slow_mnemonic == nullptr); // addsd
}

TEST("decoder - a rep-prefixed string op is slow, the bare one is not")
{
    // Matched by the prefix, not the mnemonic: Zydis spells the string and SSE forms of `movsd` the
    // same way, so a mnemonic match here would catch every scalar double move in the program.
    auto const rep = decoded({0xF3, 0xA4}); // rep movsb
    REQUIRE(rep.slow_mnemonic != nullptr);
    CHECK(cc::string_view(rep.slow_mnemonic) == "movsb");

    CHECK(decoded({0xA4}).slow_mnemonic == nullptr);                   // movsb, one byte, unremarkable
    CHECK(decoded({0xF2, 0x0F, 0x10, 0xC1}).slow_mnemonic == nullptr); // movsd xmm0, xmm1 — the SSE form
}

TEST("decoder - ordinary instructions are not slow")
{
    CHECK(decoded({0x48, 0x89, 0xC8}).slow_mnemonic == nullptr); // mov rax, rcx
    CHECK(decoded({0xC3}).slow_mnemonic == nullptr);             // ret
    CHECK(decoded({0x90}).slow_mnemonic == nullptr);             // nop
    CHECK(decoded({0xF0, 0xFF, 0x00}).slow_mnemonic == nullptr); // lock inc — atomic, but not this column
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
