#include "memory_access.hh"

#include <Zydis.h>

namespace itrace
{
namespace
{
/// Fill a Zydis register context from our snapshot. gpr[i] is register i in x86-64 encoding order,
/// which is exactly ZYDIS_REGISTER_RAX + i (RAX..R15 are contiguous in that order). rip is set for
/// the rare register-form rip use; rip-relative addressing goes through the runtime_address arg.
ZydisRegisterContext register_context_of(register_snapshot const& regs, u64 rip)
{
    ZydisRegisterContext ctx = {};
    for (int i = 0; i < gpr_count; ++i)
        ctx.values[ZYDIS_REGISTER_RAX + i] = regs.gpr[i];
    ctx.values[ZYDIS_REGISTER_RIP] = rip;
    return ctx;
}
} // namespace

cc::vector<mem_operand> decode_memory_operands(recorded_instruction const& insn, register_snapshot const& regs)
{
    cc::vector<mem_operand> out;

    if (insn.byte_count == 0)
        return out;

    ZydisDisassembledInstruction decoded = {};
    auto const status
        = ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, insn.rip, insn.bytes.data(), insn.byte_count, &decoded);
    if (!ZYAN_SUCCESS(status))
        return out;

    auto const ctx = register_context_of(regs, insn.rip);

    for (u8 i = 0; i < decoded.info.operand_count; ++i)
    {
        auto const& op = decoded.operands[i];
        if (op.type != ZYDIS_OPERAND_TYPE_MEMORY)
            continue;

        // An address computed but not dereferenced (lea) has neither action — skip it, it touches
        // no memory. Explicit and implicit accesses are both kept, so push/pop/call/ret count.
        bool const reads = (op.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0;
        bool const writes = (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
        if (!reads && !writes)
            continue;

        ZyanU64 address = 0;
        if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&decoded.info, &op, insn.rip, &ctx, &address)))
            continue;

        mem_operand acc;
        acc.address = address;
        acc.size = u16(op.size / 8);
        acc.is_read = reads;
        acc.is_write = writes;
        out.push_back(acc);
    }

    return out;
}

access_region classify_region(u64 address, u64 stack_low, u64 stack_high, cc::span<u64 const> frame_bases, isize& owner_frame)
{
    owner_frame = -1;

    // No stack bounds, or off the stack entirely: a heap allocation or a global.
    if (stack_low == 0 || address < stack_low || address >= stack_high)
        return access_region::heap;

    // On the stack. The owning frame is the innermost one whose base is still above the address —
    // a frame's own memory grows down from its base until the next inner frame begins. Bases run
    // outermost (highest) to current (lowest), so the last base greater than the address wins.
    isize best = -1;
    for (isize k = 0; k < frame_bases.size(); ++k)
        if (frame_bases[k] > address)
            best = k;

    if (best < 0)
    {
        // Above every tracked frame: a caller we never entered. Still stack, owner unknown.
        return access_region::stack;
    }

    owner_frame = best;
    return best == frame_bases.size() - 1 ? access_region::frame : access_region::stack;
}

cc::string_view region_name(access_region region)
{
    switch (region)
    {
    case access_region::heap:
        return "heap";
    case access_region::frame:
        return "frame";
    case access_region::stack:
        return "stack";
    case access_region::instructions:
        return "instructions";
    }
    return "?";
}
} // namespace itrace
