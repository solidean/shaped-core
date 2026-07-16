#pragma once

#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string.hh>

/// Windows x64 single-step instruction tracer. See tools/instruction-tracer/readme.md.
namespace itrace
{
using namespace cc::primitive_defines;

/// An x86 instruction is at most 15 bytes.
inline constexpr int max_instruction_bytes = 15;

/// The 16 general-purpose registers, in x86-64 encoding order (matches Zydis and our snapshots).
inline constexpr int gpr_count = 16;

/// gpr_names[i] names register i in encoding order.
inline constexpr char const* gpr_names[gpr_count] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", //
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15", //
};

/// What an instruction does to control flow. Drives branch annotation and the syscall stop.
enum class insn_category
{
    other,
    conditional_branch,
    unconditional_branch,
    call,
    ret,
    syscall,
};

/// The 16 GPRs plus rflags, sampled before an instruction. Only captured with --register-diffs.
struct register_snapshot
{
    cc::fixed_array<u64, gpr_count> gpr = {};
    u64 rflags = 0;
};

/// One retired instruction.
///
/// The live loop fills only rip/next_rip/rsp/bytes; `length`, `text`, `category` and the is_/…_memory
/// flags come from the decoder afterwards, and file/line/target_symbol/owner_symbol from symbol
/// enrichment. Everything past the raw capture is best-effort and stays empty/false when unavailable.
struct recorded_instruction
{
    u64 rip = 0;
    /// Where the CPU actually went next — the authority for branch annotation. 0 for the last record.
    u64 next_rip = 0;
    u64 rsp = 0;

    cc::fixed_array<u8, max_instruction_bytes> bytes = {};
    u8 byte_count = 0;

    u8 length = 0; // 0 = not decoded
    cc::string text;
    insn_category category = insn_category::other;

    /// A locked read-modify-write: a `lock` prefix, or an `xchg` with memory (locked implicitly).
    bool is_atomic = false;
    /// A call/jmp through a register or memory — a vtable, function_ref or unique_function hop.
    bool is_indirect = false;
    /// Has an explicit memory operand it reads / writes. Both are true for a read-modify-write.
    bool reads_memory = false;
    bool writes_memory = false;

    /// This instruction's name when it is one that costs tens to hundreds of cycles where the rest of
    /// the stream costs ~1 — `idiv`, `rdtsc`, a fence, a `rep`-prefixed string op. Null otherwise.
    /// A static string, valid for the process lifetime.
    ///
    /// Deliberately not a cost model: it flags that the instruction count will mislead here, and
    /// leaves the reader to look. See slow_mnemonic_of.
    char const* slow_mnemonic = nullptr;

    cc::string file;
    u32 line = 0;
    /// Where a taken transfer landed, symbolized. Only set when control actually diverged.
    cc::string target_symbol;
    /// The function containing `rip`, without an offset. Only filled when stats are requested.
    cc::string owner_symbol;
};

/// True when control did not simply fall through to the next instruction — the authority for whether
/// a conditional branch was taken. False when unknowable: an undecoded record, or the last one (whose
/// successor we never saw).
inline bool diverged(recorded_instruction const& insn)
{
    if (insn.next_rip == 0 || insn.length == 0)
        return false;

    return insn.next_rip != insn.rip + insn.length;
}

/// Why a trace stopped collecting.
enum class step_reason
{
    instruction_budget,
    returned,
    syscall,
    exception,
    process_exited,
};

/// One resolved frame of the stack captured at function entry.
struct stack_frame
{
    u64 rip = 0;
    cc::string symbol;
    cc::string module;
    cc::string file;
    u32 line = 0;
};

/// One recorded invocation of the traced function.
struct trace
{
    u32 index = 0;     // 1-based, across all threads
    u64 hit_index = 0; // 1-based breakpoint hit this trace came from
    u32 thread_id = 0;

    u64 entry_rip = 0;
    u64 return_rip = 0;
    cc::string entry_symbol;
    cc::string return_symbol;

    cc::vector<stack_frame> entry_stack;
    cc::vector<recorded_instruction> instructions;
    /// One snapshot sampled *before* each instruction, plus a trailing one holding what the last
    /// instruction left behind — so instruction i's effect is registers[i] vs registers[i+1], and
    /// size is instructions.size() + 1. The trailing entry is absent where the last instruction never
    /// retired (the syscall stop, which records the gate but refuses to step it). Empty unless
    /// --register-diffs.
    cc::vector<register_snapshot> registers;

    step_reason reason = step_reason::instruction_budget;
};
} // namespace itrace
