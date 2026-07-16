#include "trace_session.hh"

#include <clean-core/platform/win32_sanitized.hh>
#include <instruction-tracer/decode/instruction_decoder.hh>

namespace itrace
{
namespace
{
/// Read up to `max_instruction_bytes` at `address`. Short reads are fine and expected at the end of
/// a mapping, so we back off rather than fail: the decoder only needs the bytes of one instruction.
u8 read_code(void* process, u64 address, cc::fixed_array<u8, max_instruction_bytes>& out)
{
    for (int want = max_instruction_bytes; want > 0; --want)
    {
        SIZE_T read = 0;
        if (ReadProcessMemory(process, reinterpret_cast<void const*>(address), out.data(), SIZE_T(want), &read)
            && read == SIZE_T(want))
            return u8(want);
    }
    return 0;
}

register_snapshot snapshot_of(CONTEXT const& ctx)
{
    register_snapshot s;
    // x86-64 encoding order, matching gpr_names.
    s.gpr = {ctx.Rax, ctx.Rcx, ctx.Rdx, ctx.Rbx, ctx.Rsp, ctx.Rbp, ctx.Rsi, ctx.Rdi,
             ctx.R8,  ctx.R9,  ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15};
    s.rflags = ctx.EFlags;
    return s;
}
} // namespace

trace_session::trace_session(void* process, trace_config const& config) : _process(process), _config(config)
{
}

void trace_session::begin(u32 index,
                          u64 hit_index,
                          u32 thread_id,
                          void* thread_handle,
                          void const* context,
                          symbol_session const& symbols)
{
    auto const& ctx = *static_cast<CONTEXT const*>(context);

    _trace = {};
    _trace.index = index;
    _trace.hit_index = hit_index;
    _trace.thread_id = thread_id;
    _trace.entry_rip = ctx.Rip;
    _entry_rsp = ctx.Rsp;

    // The prologue has not run, so [rsp] is exactly the return address. This is the one moment where
    // reading it needs no unwind info at all.
    u64 return_rip = 0;
    SIZE_T read = 0;
    if (ReadProcessMemory(_process, reinterpret_cast<void const*>(ctx.Rsp), &return_rip, sizeof(return_rip), &read)
        && read == sizeof(return_rip))
        _trace.return_rip = return_rip;

    _trace.entry_symbol = symbols.describe(ctx.Rip);
    if (_trace.return_rip != 0)
        _trace.return_symbol = symbols.describe(_trace.return_rip);

    if (_config.capture_stack)
        _trace.entry_stack = symbols.walk_stack(thread_handle, context);

    _active = true;
    record_at(context);
}

bool trace_session::record_at(void const* context)
{
    auto const& ctx = *static_cast<CONTEXT const*>(context);

    recorded_instruction insn;
    insn.rip = ctx.Rip;
    insn.rsp = ctx.Rsp;
    insn.byte_count = read_code(_process, ctx.Rip, insn.bytes);

    if (insn.byte_count == 0)
        return false;

    _trace.instructions.push_back(cc::move(insn));
    if (_config.capture_registers)
        _trace.registers.push_back(snapshot_of(ctx));

    return true;
}

void trace_session::record_final_registers(void const* context)
{
    if (!_config.capture_registers)
        return;

    _trace.registers.push_back(snapshot_of(*static_cast<CONTEXT const*>(context)));
}

bool trace_session::on_step(void const* context)
{
    CC_ASSERT(_active, "on_step outside an active trace");

    auto const& ctx = *static_cast<CONTEXT const*>(context);

    // The step just retired the last recorded instruction, so this is where it actually went.
    if (!_trace.instructions.empty())
        _trace.instructions.back().next_rip = ctx.Rip;

    // The entry frame returned. The rsp guard rejects a recursive call returning to the same address
    // at a deeper frame — only the original frame has popped its return address by now.
    if (_config.until_return && _trace.return_rip != 0 && ctx.Rip == _trace.return_rip
        && ctx.Rsp >= _entry_rsp + sizeof(u64))
    {
        record_final_registers(context);
        _trace.reason = step_reason::returned;
        _active = false;
        return false;
    }

    if (u32(_trace.instructions.size()) >= _config.max_instructions)
    {
        record_final_registers(context);
        _trace.reason = step_reason::instruction_budget;
        _active = false;
        return false;
    }

    if (!record_at(context))
    {
        // The step itself landed; only reading the *next* instruction failed, so the state here is
        // still the last recorded instruction's result.
        record_final_registers(context);
        _trace.reason = step_reason::exception;
        _active = false;
        return false;
    }

    // Record the syscall but stop before stepping it: single-stepping a kernel transition is not
    // something we want to reason about.
    auto const& last = _trace.instructions.back();
    if (_config.stop_at_syscall && is_syscall_bytes(cc::span<u8 const>(last.bytes.data(), last.byte_count)))
    {
        _trace.reason = step_reason::syscall;
        _active = false;
        return false;
    }

    return true;
}

void trace_session::abort(step_reason reason)
{
    _trace.reason = reason;
    _active = false;
}
} // namespace itrace
