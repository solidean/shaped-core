#include "instruction_decoder.hh"

#include <Zydis.h>
#include <clean-core/string/string_view.hh>

namespace itrace
{
namespace
{
insn_category category_of(ZydisInstructionCategory category)
{
    switch (category)
    {
    case ZYDIS_CATEGORY_COND_BR:
        return insn_category::conditional_branch;
    case ZYDIS_CATEGORY_UNCOND_BR:
        return insn_category::unconditional_branch;
    case ZYDIS_CATEGORY_CALL:
        return insn_category::call;
    case ZYDIS_CATEGORY_RET:
        return insn_category::ret;
    case ZYDIS_CATEGORY_SYSCALL:
    case ZYDIS_CATEGORY_INTERRUPT:
        return insn_category::syscall;
    default:
        return insn_category::other;
    }
}

/// A locked RMW. The `lock` prefix is the usual form, but `xchg` with a memory operand is locked
/// implicitly — no prefix, no ZYDIS_ATTRIB_HAS_LOCK — and that is how an atomic store/exchange
/// compiles, so matching only the prefix silently undercounts.
bool is_atomic_insn(ZydisDisassembledInstruction const& d)
{
    if ((d.info.attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0)
        return true;

    if (d.info.mnemonic != ZYDIS_MNEMONIC_XCHG)
        return false;

    for (u8 i = 0; i < d.info.operand_count; ++i)
        if (d.operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
            return true;

    return false;
}

/// Through a register or memory rather than a rel32 immediate. Only meaningful for a call/jmp.
bool is_indirect_transfer(ZydisDisassembledInstruction const& d)
{
    for (u8 i = 0; i < d.info.operand_count; ++i)
    {
        auto const& op = d.operands[i];
        if (op.visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT)
            continue;

        // The first explicit operand is the destination; anything but an immediate is indirect.
        return op.type != ZYDIS_OPERAND_TYPE_IMMEDIATE;
    }
    return false;
}

/// Instructions that are categorically not single-cycle — tens to hundreds of cycles each, where
/// everything around them is ~1. Returns Zydis's static mnemonic string, or nullptr.
///
/// **Not a cost model, on purpose.** Exact latencies are microarchitecture-specific (idiv r64 is ~18
/// cycles on one part and different on the next), so nothing here estimates or weighs. Membership is
/// the claim: "the instruction count will mislead you here, go look". The moment this returns
/// something that reads like a cycle count, someone will trust it as one.
///
/// The cost this CANNOT see is the one that usually matters: a `mov` that misses to DRAM is 200+
/// cycles and is indistinguishable from an L1 hit. This finds landmines in the opcode stream, not
/// where the time went.
char const* slow_mnemonic_of(ZydisDisassembledInstruction const& d)
{
    switch (d.info.mnemonic)
    {
    // Integer divide — the one that sneaks in from a `%` on a non-power-of-two.
    case ZYDIS_MNEMONIC_DIV:
    case ZYDIS_MNEMONIC_IDIV:
    // Float divide and square root.
    case ZYDIS_MNEMONIC_DIVSS:
    case ZYDIS_MNEMONIC_DIVSD:
    case ZYDIS_MNEMONIC_DIVPS:
    case ZYDIS_MNEMONIC_DIVPD:
    case ZYDIS_MNEMONIC_VDIVSS:
    case ZYDIS_MNEMONIC_VDIVSD:
    case ZYDIS_MNEMONIC_VDIVPS:
    case ZYDIS_MNEMONIC_VDIVPD:
    case ZYDIS_MNEMONIC_SQRTSS:
    case ZYDIS_MNEMONIC_SQRTSD:
    case ZYDIS_MNEMONIC_SQRTPS:
    case ZYDIS_MNEMONIC_SQRTPD:
    case ZYDIS_MNEMONIC_VSQRTSS:
    case ZYDIS_MNEMONIC_VSQRTSD:
    case ZYDIS_MNEMONIC_VSQRTPS:
    case ZYDIS_MNEMONIC_VSQRTPD:
    // Serializing / timing — what profiling code is built out of.
    case ZYDIS_MNEMONIC_CPUID:
    case ZYDIS_MNEMONIC_RDTSC:
    case ZYDIS_MNEMONIC_RDTSCP:
    case ZYDIS_MNEMONIC_RDPMC:
    case ZYDIS_MNEMONIC_RDRAND:
    case ZYDIS_MNEMONIC_RDSEED:
    case ZYDIS_MNEMONIC_XGETBV:
    case ZYDIS_MNEMONIC_WBINVD:
    // Fences and the spin-wait hint: a `pause` means a contended lock is spinning.
    case ZYDIS_MNEMONIC_MFENCE:
    case ZYDIS_MNEMONIC_LFENCE:
    case ZYDIS_MNEMONIC_SFENCE:
    case ZYDIS_MNEMONIC_PAUSE:
    case ZYDIS_MNEMONIC_CLFLUSH:
    case ZYDIS_MNEMONIC_CLFLUSHOPT:
    // x87 transcendentals: microcoded, and nobody means to emit them.
    case ZYDIS_MNEMONIC_FDIV:
    case ZYDIS_MNEMONIC_FSQRT:
    case ZYDIS_MNEMONIC_FSIN:
    case ZYDIS_MNEMONIC_FCOS:
    case ZYDIS_MNEMONIC_FPTAN:
    case ZYDIS_MNEMONIC_FYL2X:
        return ZydisMnemonicGetString(d.info.mnemonic);
    default:
        break;
    }

    // A rep-prefixed string op costs whatever rcx says. Matched by prefix rather than by mnemonic
    // because Zydis spells the string and SSE forms of `movsd` with the same one.
    constexpr auto rep_prefixes = ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE;
    if ((d.info.attributes & rep_prefixes) != 0)
        return ZydisMnemonicGetString(d.info.mnemonic);

    // Gather/scatter: one memory access per lane. A family far too large to list, but named uniformly.
    auto const* const name = ZydisMnemonicGetString(d.info.mnemonic);
    if (name != nullptr)
    {
        cc::string_view const n = name;
        if (n.starts_with("vgather") || n.starts_with("vpgather") || n.starts_with("vscatter")
            || n.starts_with("vpscatter"))
            return name;
    }

    return nullptr;
}

/// Whether any explicit memory operand is read / written. Explicit only: the implicit stack traffic
/// of a push/pop/call would otherwise mark nearly everything, drowning out the pointer chases.
void fill_memory_access(ZydisDisassembledInstruction const& d, recorded_instruction& insn)
{
    for (u8 i = 0; i < d.info.operand_count; ++i)
    {
        auto const& op = d.operands[i];
        if (op.visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT || op.type != ZYDIS_OPERAND_TYPE_MEMORY)
            continue;

        if ((op.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0)
            insn.reads_memory = true;
        if ((op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0)
            insn.writes_memory = true;
    }
}
} // namespace

bool is_syscall_bytes(cc::span<u8 const> bytes)
{
    // syscall (0F 05) and sysenter (0F 34).
    if (bytes.size() >= 2 && bytes[0] == 0x0F && (bytes[1] == 0x05 || bytes[1] == 0x34))
        return true;

    // int 0x2e — the legacy transition.
    if (bytes.size() >= 2 && bytes[0] == 0xCD && bytes[1] == 0x2E)
        return true;

    return false;
}

void instruction_decoder::decode_one(recorded_instruction& insn) const
{
    if (insn.byte_count == 0)
        return;

    ZydisDisassembledInstruction decoded = {};
    auto const status
        = ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, insn.rip, insn.bytes.data(), insn.byte_count, &decoded);
    if (!ZYAN_SUCCESS(status))
        return;

    insn.length = u8(decoded.info.length);
    insn.text = decoded.text;
    insn.category = category_of(decoded.info.meta.category);

    // ZYDIS_CATEGORY_INTERRUPT covers every `int n` plus int3/into; only the syscall gate is one.
    if (insn.category == insn_category::syscall
        && !is_syscall_bytes(cc::span<u8 const>(insn.bytes.data(), insn.byte_count)))
        insn.category = insn_category::other;

    insn.is_atomic = is_atomic_insn(decoded);
    insn.is_indirect = (insn.category == insn_category::call || insn.category == insn_category::unconditional_branch)
                    && is_indirect_transfer(decoded);
    insn.slow_mnemonic = slow_mnemonic_of(decoded);
    fill_memory_access(decoded, insn);
}

void instruction_decoder::decode(cc::span<recorded_instruction> instructions) const
{
    for (auto& insn : instructions)
        decode_one(insn);
}
} // namespace itrace
