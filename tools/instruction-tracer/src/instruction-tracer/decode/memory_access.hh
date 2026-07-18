#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <instruction-tracer/debug/trace_record.hh>

namespace itrace
{
/// One memory operand of an instruction, resolved to a runtime effective address but not yet
/// classified into a region or symbolized.
struct mem_operand
{
    u64 address = 0;
    u16 size = 0; // bytes
    bool is_read = false;
    bool is_write = false;
};

/// Resolve every memory operand of one instruction to an effective address, from the register
/// snapshot sampled before it ran (base + index*scale + disp, and rip-relative). Explicit and
/// implicit operands both — the implicit stack traffic of push/pop/call/ret is included, and lands
/// in the `frame` region once classified. Pure Zydis: no debuggee, no symbols. Empty when the bytes
/// do not decode or the instruction touches no memory.
///
/// gs/fs segment-relative operands (TLS) are resolved without their segment base, so their address
/// is the offset alone — a known limitation, see readme.md.
cc::vector<mem_operand> decode_memory_operands(recorded_instruction const& insn, register_snapshot const& regs);

/// Classify an address as heap / frame / stack. `frame_bases` holds the return-address-slot rsp of
/// every active frame, outermost (highest address) first and the current function last; it is what
/// separates the current frame's own memory from a caller's. Returns:
///   - heap when the address is outside [stack_low, stack_high) (or no stack bounds are known),
///   - frame when it belongs to the current (innermost) frame,
///   - stack when it belongs to a shallower frame — a stack array reached through a passed-in span.
/// `owner_frame` receives the index into `frame_bases` of the owning frame, or -1 when the address
/// sits above every tracked frame (an untracked caller) or is heap.
access_region classify_region(u64 address,
                              u64 stack_low,
                              u64 stack_high,
                              cc::span<u64 const> frame_bases,
                              isize& owner_frame);

/// "heap" / "frame" / "stack" / "instructions".
cc::string_view region_name(access_region region);
} // namespace itrace
