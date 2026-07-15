#include "debug_session.hh"

#include <clean-core/platform/win32_sanitized.hh>
#include <clean-core/string/conversion.hh>
#include <clean-core/string/format.hh>

namespace itrace
{
namespace
{
/// EFLAGS.TF — the CPU clears it after every step, so it must be re-set for each one.
constexpr u32 trap_flag = 0x100;

cc::vector<char16_t> to_utf16_z(cc::string_view s)
{
    auto out = cc::utf8_to_utf16(s);
    out.push_back(u'\0');
    return out;
}

/// Resolve `exe` to a full path for CreateProcessW's lpApplicationName.
///
/// Needed because passing only a command line makes CreateProcessW parse the executable out of it,
/// which does not accept a relative forward-slash path — exactly what a shell hands us. Empty when
/// the path does not name an existing file, in which case the caller lets CreateProcessW do its own
/// PATH search instead (so a bare "foo.exe" still works).
cc::vector<char16_t> resolve_exe_path(cc::string_view exe)
{
    auto exe_w = to_utf16_z(exe);

    wchar_t full[MAX_PATH] = {};
    auto const len = GetFullPathNameW(reinterpret_cast<wchar_t const*>(exe_w.data()), MAX_PATH, full, nullptr);
    if (len == 0 || len >= MAX_PATH)
        return {};

    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
        return {};

    cc::vector<char16_t> out;
    for (DWORD i = 0; i <= len; ++i) // includes the terminator
        out.push_back(char16_t(full[i]));

    return out;
}

/// Quote one argv element per the Win32 (CommandLineToArgvW) rules.
void append_quoted(cc::string& out, cc::string_view arg)
{
    bool const needs_quotes = arg.empty() || arg.contains(' ') || arg.contains('\t') || arg.contains('"');
    if (!needs_quotes)
    {
        out += arg;
        return;
    }

    out += '"';
    int backslashes = 0;
    for (char const c : arg)
    {
        if (c == '\\')
        {
            ++backslashes;
            continue;
        }

        if (c == '"')
        {
            // Backslashes before a quote must be doubled, and the quote escaped.
            for (int i = 0; i < backslashes * 2 + 1; ++i)
                out += '\\';
            backslashes = 0;
        }
        else
        {
            for (int i = 0; i < backslashes; ++i)
                out += '\\';
            backslashes = 0;
        }
        out += c;
    }

    // Trailing backslashes would escape our closing quote.
    for (int i = 0; i < backslashes * 2; ++i)
        out += '\\';
    out += '"';
}

cc::string build_command_line(cc::string_view exe, cc::span<cc::string const> args)
{
    cc::string out;
    append_quoted(out, exe);
    for (auto const& a : args)
    {
        out += ' ';
        append_quoted(out, a);
    }
    return out;
}

/// The image's SizeOfImage, read from its in-memory PE header. 0 when it cannot be read.
u64 read_image_size(void* process, u64 base)
{
    IMAGE_DOS_HEADER dos = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void const*>(base), &dos, sizeof(dos), &read)
        || read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    IMAGE_NT_HEADERS64 nt = {};
    if (!ReadProcessMemory(process, reinterpret_cast<void const*>(base + u64(dos.e_lfanew)), &nt, sizeof(nt), &read)
        || read != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE)
        return 0;

    return nt.OptionalHeader.SizeOfImage;
}

/// The on-disk path behind a debug event's hFile. Empty when it cannot be resolved.
cc::string path_of_handle(void* file_handle)
{
    if (file_handle == nullptr || file_handle == INVALID_HANDLE_VALUE)
        return {};

    wchar_t buffer[MAX_PATH] = {};
    auto const len = GetFinalPathNameByHandleW(file_handle, buffer, MAX_PATH, FILE_NAME_NORMALIZED);
    if (len == 0 || len >= MAX_PATH)
        return {};

    cc::string out;
    for (DWORD i = 0; i < len; ++i)
        out += char(buffer[i] < 128 ? buffer[i] : '?');

    // GetFinalPathNameByHandleW returns the \\?\ prefixed form.
    if (out.subview({.offset = 0, .size = cc::min<isize>(4, out.size())}) == "\\\\?\\")
        out = out.substring({.start = 4, .end = out.size()});

    return out;
}

bool get_context(void* thread, CONTEXT& ctx)
{
    ctx = {};
    ctx.ContextFlags = CONTEXT_FULL | CONTEXT_CONTROL | CONTEXT_INTEGER;
    return GetThreadContext(thread, &ctx) != 0;
}

void set_trap_flag(void* thread, CONTEXT& ctx, bool enabled)
{
    if (enabled)
        ctx.EFlags |= trap_flag;
    else
        ctx.EFlags &= ~trap_flag;

    SetThreadContext(thread, &ctx);
}
} // namespace

debug_session::debug_session(debug_config config) : _config(cc::move(config))
{
}

debug_session::~debug_session()
{
    // The tracer/breakpoint/symbols reference _process, so drop them before closing it.
    _tracer = nullptr;
    _breakpoint = nullptr;
    _symbols = nullptr;

    // Modules still loaded when the debuggee died never saw an unload event.
    for (auto const& [base, handle] : _module_files)
        CloseHandle(handle);

    if (_process != nullptr)
        CloseHandle(_process);
}

void debug_session::register_module(void* file_handle, u64 base)
{
    module_info info;
    info.base = base;
    info.size = read_image_size(_process, base);
    info.path = path_of_handle(file_handle);
    info.name = info.path.empty() ? cc::format("{:#x}", base) : cc::string(path_file_name(info.path));

    _modules.add(cc::move(info));

    if (auto const* m = _modules.find_by_address(base))
        _symbols->on_module_loaded(*m, file_handle);

    // Held, not closed: dbghelp reads the PDB through this handle on its first deferred query.
    if (file_handle != nullptr)
        _module_files[base] = file_handle;
}

void debug_session::unregister_module(u64 base)
{
    _symbols->on_module_unloaded(base);
    _modules.remove(base);

    if (auto* const handle = _module_files.get_or(base, nullptr))
    {
        CloseHandle(handle);
        _module_files.erase(base);
    }
}

void debug_session::try_resolve_and_arm()
{
    if (_resolved)
        return;

    auto address = _symbols->resolve(_config.target);
    if (address.has_error())
    {
        // Not found yet is not fatal — the symbol may live in a DLL that has not loaded. Keep the
        // last error so run() can report something useful if it never does resolve.
        _resolve_error = address.error();

        // Ambiguity is different: a later module can only add candidates, never remove them. Give
        // up now rather than running the debuggee to completion first.
        if (!address.error().candidates.empty())
            _state = state::finished;

        return;
    }

    _resolve_error = {};
    _breakpoint = cc::make_unique<entry_breakpoint>(_process, address.value());
    _resolved = _breakpoint->arm();
}

u32 debug_session::on_create_process(void const* raw, u32 thread_id)
{
    auto const& info = *static_cast<CREATE_PROCESS_DEBUG_INFO const*>(raw);

    _main_thread = info.hThread;
    _threads[thread_id] = info.hThread;

    _symbols = cc::make_unique<symbol_session>(_process, _modules);

    register_module(info.hFile, u64(info.lpBaseOfImage));
    try_resolve_and_arm();

    return DBG_CONTINUE;
}

u32 debug_session::on_load_dll(void const* raw)
{
    auto const& info = *static_cast<LOAD_DLL_DEBUG_INFO const*>(raw);

    register_module(info.hFile, u64(info.lpBaseOfDll));

    // The target may live here.
    try_resolve_and_arm();

    return DBG_CONTINUE;
}

u32 debug_session::on_breakpoint_hit(u32 thread_id, void* thread)
{
    CONTEXT ctx;
    if (!get_context(thread, ctx))
        return DBG_CONTINUE;

    // int3 leaves rip one past the trap byte.
    ctx.Rip -= 1;
    SetThreadContext(thread, &ctx);

    _breakpoint->disarm();
    ++_hit_index;

    _traced_thread = thread_id;

    if (_hit_index <= _config.skip || u32(_traces.size()) >= _config.traces)
    {
        // A warm-up hit: step the displaced instruction so we can re-arm behind it.
        _state = state::stepping_off_breakpoint;
        set_trap_flag(thread, ctx, true);
        return DBG_CONTINUE;
    }

    _tracer = cc::make_unique<trace_session>(_process, _config.trace);
    _tracer->begin(u32(_traces.size()) + 1, _hit_index, thread_id, thread, &ctx, *_symbols);

    _state = state::tracing;
    set_trap_flag(thread, ctx, true);
    return DBG_CONTINUE;
}

void debug_session::finish_trace()
{
    auto t = _tracer->take();
    _tracer = nullptr;

    // Symbolize now: the PDB session dies with the debuggee.
    if (_on_trace.is_valid())
        _on_trace(t, *_symbols);

    _traces.push_back(cc::move(t));

    if (u32(_traces.size()) >= _config.traces && _config.terminate_after_traces)
    {
        _state = state::finished;
        return;
    }

    // More to collect (or we are letting the debuggee run on): re-arm and wait for the next hit.
    _breakpoint->arm();
    _state = state::waiting_for_entry;
}

u32 debug_session::on_single_step(u32 thread_id, void* thread)
{
    CONTEXT ctx;
    if (!get_context(thread, ctx))
        return DBG_CONTINUE;

    if (_state == state::stepping_off_breakpoint)
    {
        // The displaced instruction has retired; the int3 can go back.
        _breakpoint->arm();
        _state = state::waiting_for_entry;
        set_trap_flag(thread, ctx, false);
        return DBG_CONTINUE;
    }

    if (_state != state::tracing || thread_id != _traced_thread)
        return DBG_CONTINUE;

    if (_tracer->on_step(&ctx))
    {
        // TF is cleared by the CPU after each step.
        set_trap_flag(thread, ctx, true);
        return DBG_CONTINUE;
    }

    set_trap_flag(thread, ctx, false);
    finish_trace();

    if (_state == state::finished)
        TerminateProcess(_process, 0);

    return DBG_CONTINUE;
}

u32 debug_session::on_exception(void const* raw, u32 thread_id)
{
    auto const& info = *static_cast<EXCEPTION_DEBUG_INFO const*>(raw);
    auto const code = info.ExceptionRecord.ExceptionCode;

    auto* const thread = _threads.get_or(thread_id, nullptr);
    if (thread == nullptr)
        return DBG_EXCEPTION_NOT_HANDLED;

    if (code == EXCEPTION_BREAKPOINT)
    {
        auto const at = u64(info.ExceptionRecord.ExceptionAddress);

        // The loader fires one int3 of its own before main runs; and if the target never resolved,
        // no breakpoint of ours exists at all.
        if (!_resolved || _breakpoint == nullptr || at != _breakpoint->address())
            return DBG_CONTINUE;

        return on_breakpoint_hit(thread_id, thread);
    }

    if (code == EXCEPTION_SINGLE_STEP)
        return on_single_step(thread_id, thread);

    // A real fault. Stop any trace in flight, then let the debuggee's own handlers deal with it.
    if (_state == state::tracing && thread_id == _traced_thread && _tracer != nullptr)
    {
        _tracer->abort(step_reason::exception);
        finish_trace();
    }

    return DBG_EXCEPTION_NOT_HANDLED;
}

cc::result<cc::vector<trace>, symbol_error> debug_session::run(cc::function_ref<void(trace&, symbol_session const&)> on_trace)
{
    _on_trace = on_trace;

    auto const command_line = build_command_line(_config.exe, _config.args);
    auto command_line_w = to_utf16_z(command_line);

    // Empty => let CreateProcessW search PATH for a bare name.
    auto exe_path_w = resolve_exe_path(_config.exe);
    auto* const application_name = exe_path_w.empty() ? nullptr : reinterpret_cast<wchar_t const*>(exe_path_w.data());

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info = {};

    // DEBUG_ONLY_THIS_PROCESS: we trace the target, not whatever it spawns.
    if (!CreateProcessW(application_name, reinterpret_cast<wchar_t*>(command_line_w.data()), nullptr, nullptr, FALSE,
                        DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &startup, &process_info))
        return cc::error(symbol_error{cc::format("failed to launch '{}' (error {})", _config.exe, GetLastError()), {}});

    _process = process_info.hProcess;
    CloseHandle(process_info.hThread);

    for (;;)
    {
        DEBUG_EVENT event = {};
        if (!WaitForDebugEvent(&event, INFINITE))
            break;

        u32 status = DBG_CONTINUE;

        switch (event.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            status = on_create_process(&event.u.CreateProcessInfo, event.dwThreadId);
            break;

        case CREATE_THREAD_DEBUG_EVENT:
            _threads[event.dwThreadId] = event.u.CreateThread.hThread;
            break;

        case EXIT_THREAD_DEBUG_EVENT:
            _threads.erase(event.dwThreadId);
            break;

        case LOAD_DLL_DEBUG_EVENT:
            status = on_load_dll(&event.u.LoadDll);
            break;

        case UNLOAD_DLL_DEBUG_EVENT:
            unregister_module(u64(event.u.UnloadDll.lpBaseOfDll));
            break;

        case EXCEPTION_DEBUG_EVENT:
            status = on_exception(&event.u.Exception, event.dwThreadId);
            break;

        case EXIT_PROCESS_DEBUG_EVENT:
            // A trace still in flight never finished; keep what it got. Symbolize before the
            // continue below lets the process go — this is the last moment the PDB session is live.
            if (_tracer != nullptr && _tracer->is_active())
            {
                _tracer->abort(step_reason::process_exited);
                finish_trace();
            }
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
            goto done;

        default:
            break;
        }

        // An ambiguous target is fatal; nothing is worth running the debuggee for.
        if (_state == state::finished && _traces.empty())
            TerminateProcess(_process, 0);

        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
    }

done:
    // Nothing was ever recorded and the target never resolved — that is the interesting failure.
    if (_traces.empty() && !_resolved && _resolve_error.has_value())
        return cc::error(_resolve_error.value());

    return cc::move(_traces);
}
} // namespace itrace
