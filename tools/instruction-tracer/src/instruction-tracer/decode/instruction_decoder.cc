#include "instruction_decoder.hh"

#include <Zydis.h>

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
}

void instruction_decoder::decode(cc::span<recorded_instruction> instructions) const
{
    for (auto& insn : instructions)
        decode_one(insn);
}
} // namespace itrace
