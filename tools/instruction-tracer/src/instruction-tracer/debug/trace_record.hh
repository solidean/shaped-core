#pragma once

#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string.hh>
#include <instruction-tracer/fwd.hh>

namespace itrace
{
struct memory_access;
struct recorded_instruction;
struct register_snapshot;
struct stack_frame;
struct trace;
} // namespace itrace

/// Windows x64 single-step instruction tracer.
/// See tools/instruction-tracer/readme.md.
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

/// What an instruction does to control flow.
/// Drives branch annotation and the syscall stop.
enum class insn_category
{
    other,
    conditional_branch,
    unconditional_branch,
    call,
    ret,
    syscall,
};

} // namespace itrace

/// The 16 GPRs plus rflags, sampled before an instruction.
/// Captured with --register-diffs, any memory section or --html: the effective-address computation reads base/index registers from here.
struct itrace::register_snapshot
{
    cc::fixed_array<u64, gpr_count> gpr = {};
    u64 rflags = 0;
};

namespace itrace
{

/// Where a touched address lives.
///
/// frame is the executing function's own stack frame: its locals, spills, and the return-address / saved-register machinery.
/// stack is *another* function's stack — the case that matters when a stack array is passed around as a span and reached through a pointer.
/// instructions is code memory, the instruction fetch itself, giving an I-cache footprint when opted in.
/// heap is everything else: dynamic allocations and globals, and a global keeps its name in the access.
enum class access_region
{
    heap,
    frame,
    stack,
    instructions,
};

} // namespace itrace

/// One memory location an instruction touched, with the effective address resolved from the register snapshot taken before the instruction ran.
///
/// Every memory operand is recorded: explicit data operands, the implicit stack traffic of push/pop/call/ret (which lands in `frame`), and the instruction fetch.
/// Noise is dropped by region *filtering* at print time, never by omission here, so one capture serves every region selection.
struct itrace::memory_access
{
    u64 address = 0;
    u16 size = 0; // bytes
    bool is_read = false;
    bool is_write = false;
    access_region region = access_region::heap;
    /// A global's name for a heap-region hit, the function owning the frame for a stack/frame hit, or the containing function for an instruction fetch.
    /// Empty when nothing is known.
    cc::string symbol;
};

/// One retired instruction.
///
/// The live loop fills only rip/next_rip/rsp/bytes.
/// `length`, `text`, `category` and the is_/…_memory flags come from the decoder afterwards, and file/line/target_symbol/owner_symbol from symbol enrichment.
/// Everything past the raw capture is best-effort and stays empty/false when unavailable.
struct itrace::recorded_instruction
{
    u64 rip = 0;
    /// Where the CPU actually went next — the authority for branch annotation.
    /// 0 for the last record.
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
    /// Has an explicit memory operand it reads / writes.
    /// Both are true for a read-modify-write.
    bool reads_memory = false;
    bool writes_memory = false;

    /// This instruction's name when it is one of the categorically-not-single-cycle ones — `idiv`, `rdtsc`, a fence, a `rep`-prefixed string op.
    /// Null otherwise, and a static string valid for the process lifetime.
    /// What the column claims, and what it cannot see, is the readme's `slow` section.
    char const* slow_mnemonic = nullptr;

    cc::string file;
    u32 line = 0;
    /// Where a taken transfer landed, symbolized.
    /// Only set when control actually diverged.
    cc::string target_symbol;
    /// The function containing `rip`, without an offset.
    /// Filled whenever a per-symbol table or any memory section is requested, and always for --html.
    cc::string owner_symbol;

    /// Every memory location this instruction touched, resolved from the before-instruction register snapshot.
    /// Filled only for a memory section or --html; empty otherwise.
    cc::vector<memory_access> memory_accesses;
};

namespace itrace
{

/// True when control did not simply fall through to the next instruction — the authority for whether a conditional branch was taken.
/// False when unknowable: an undecoded record, or the last one, whose successor we never saw.
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

} // namespace itrace

/// One resolved frame of the stack captured at function entry.
struct itrace::stack_frame
{
    u64 rip = 0;
    cc::string symbol;
    cc::string module;
    cc::string file;
    u32 line = 0;
};

/// One recorded invocation of the traced function.
struct itrace::trace
{
    u32 index = 0;     // 1-based, across all threads
    u64 hit_index = 0; // 1-based breakpoint hit this trace came from
    u32 thread_id = 0;

    u64 entry_rip = 0;
    u64 return_rip = 0;
    cc::string entry_symbol;
    cc::string return_symbol;

    /// The traced thread's stack reservation [low, high), captured at entry.
    /// Lets memory enrichment tell a stack address from a heap/global one.
    /// Both 0 unless trace_config::capture_registers was set — the same flag gates these bounds — and classification reads [0, 0) as "no stack known".
    u64 stack_low = 0;
    u64 stack_high = 0;

    cc::vector<stack_frame> entry_stack;
    cc::vector<recorded_instruction> instructions;
    /// One snapshot sampled *before* each instruction, plus a trailing one holding what the last instruction left behind.
    /// So instruction i's effect is registers[i] vs registers[i+1], and size is instructions.size() + 1.
    /// The trailing entry is absent wherever the last instruction never retired: the syscall stop, and any stop that came in through trace_session::abort.
    /// `reason` does not tell the two apart, so size against instructions.size() is the only authority.
    /// Empty unless registers were captured at all.
    cc::vector<register_snapshot> registers;

    step_reason reason = step_reason::instruction_budget;
};
