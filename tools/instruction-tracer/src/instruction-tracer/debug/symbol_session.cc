#include "symbol_session.hh"

#include <clean-core/platform/win32_sanitized.hh>

// NOTE: must be _after_ windows.h
#include <DbgHelp.h>
#include <clean-core/string/format.hh>

namespace itrace
{
namespace
{
/// SYMBOL_INFO with its variable-length name buffer, which dbghelp expects to trail the struct.
struct symbol_buffer
{
    SYMBOL_INFO info = {};
    char name[MAX_SYM_NAME] = {};

    symbol_buffer()
    {
        info.SizeOfStruct = sizeof(SYMBOL_INFO);
        info.MaxNameLen = MAX_SYM_NAME;
    }
};

struct enum_context
{
    cc::vector<symbol_match>* out;
    module_registry const* modules;
};

BOOL CALLBACK collect_symbol(SYMBOL_INFO* info, ULONG, void* raw)
{
    auto* const ctx = static_cast<enum_context*>(raw);

    symbol_match match;
    match.address = info->Address;
    match.name = cc::string_view(info->Name, isize(info->NameLen));
    if (auto const* m = ctx->modules->find_by_address(info->Address))
        match.module = m->name;

    ctx->out->push_back(cc::move(match));
    return TRUE;
}

/// Drop duplicate addresses: dbghelp reports a symbol once per matching record, and the same
/// function can surface as several records. Ambiguity is about distinct addresses, not names.
void dedupe_by_address(cc::vector<symbol_match>& matches)
{
    cc::vector<symbol_match> unique;
    for (auto& m : matches)
    {
        bool seen = false;
        for (auto const& u : unique)
            if (u.address == m.address)
            {
                seen = true;
                break;
            }

        if (!seen)
            unique.push_back(cc::move(m));
    }
    matches = cc::move(unique);
}
} // namespace

symbol_session::symbol_session(void* process, module_registry const& modules) : _process(process), _modules(modules)
{
    SymSetOptions(SYMOPT_LOAD_LINES                                   // SymGetLineFromAddr64 needs this (--source)
                  | SYMOPT_UNDNAME                                    // "foo::bar" rather than "?bar@foo@@YAXXZ"
                  | SYMOPT_DEFERRED_LOADS                             // start fast; a PDB loads on its first query
                  | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS); // never block on a symbol-server dialog

    // fInvadeProcess=FALSE: we register modules from the loader's own events instead, which is the
    // correct live-debugger pattern — invading races the loader at startup.
    SymInitializeW(_process, nullptr, FALSE);
}

symbol_session::~symbol_session()
{
    SymCleanup(_process);
}

void symbol_session::on_module_loaded(module_info const& module, void* file_handle)
{
    SymLoadModuleExW(_process, file_handle, nullptr, nullptr, module.base, DWORD(module.size), nullptr, 0);
}

void symbol_session::on_module_unloaded(u64 base)
{
    SymUnloadModule64(_process, base);
}

cc::optional<symbol_match> symbol_session::symbol_at(u64 address) const
{
    symbol_buffer buffer;
    DWORD64 displacement = 0;
    if (!SymFromAddr(_process, address, &displacement, &buffer.info))
        return {};

    symbol_match match;
    match.address = buffer.info.Address;
    match.name = cc::string_view(buffer.info.Name, isize(buffer.info.NameLen));
    if (auto const* m = _modules.find_by_address(address))
        match.module = m->name;

    return match;
}

cc::optional<source_line> symbol_session::line_at(u64 address) const
{
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    DWORD displacement = 0;
    if (!SymGetLineFromAddr64(_process, address, &displacement, &line))
        return {};

    source_line out;
    out.file = line.FileName;
    out.line = u32(line.LineNumber);
    return out;
}

cc::string symbol_session::describe(u64 address) const
{
    auto const sym = symbol_at(address);
    if (!sym.has_value())
    {
        if (auto const* m = _modules.find_by_address(address))
            return cc::format("{}+{:#x}", m->name, address - m->base);

        return cc::format("{:#018x}", address);
    }

    auto const& s = sym.value();
    auto const offset = address - s.address;
    auto const name = s.module.empty() ? s.name : cc::format("{}!{}", s.module, s.name);
    return offset == 0 ? name : cc::format("{}+{:#x}", name, offset);
}

cc::result<u64, symbol_error> symbol_session::resolve(target_spec const& spec) const
{
    // Address forms need no symbols at all.
    if (spec.form == target_spec::kind::address)
        return spec.address;

    if (spec.form == target_spec::kind::module_offset)
    {
        auto const* m = _modules.find_by_name(spec.module);
        if (m == nullptr)
            return cc::error(symbol_error{cc::format("no loaded module named '{}'", spec.module), {}});

        return m->base + spec.address;
    }

    // A module-qualified symbol scopes the search; a bare one sweeps every module (base 0).
    u64 scope_base = 0;
    if (spec.form == target_spec::kind::module_symbol)
    {
        auto const* m = _modules.find_by_name(spec.module);
        if (m == nullptr)
            return cc::error(symbol_error{cc::format("no loaded module named '{}'", spec.module), {}});

        scope_base = m->base;
    }

    // Exact match first: an exact name is unambiguous by construction, so a symbol that also happens
    // to be a substring of others still resolves. (c_str_materialize mutates, hence the local copy.)
    {
        auto symbol_z = spec.symbol;
        symbol_buffer buffer;
        if (SymFromName(_process, symbol_z.c_str_materialize(), &buffer.info))
        {
            bool const in_scope = scope_base == 0 || (buffer.info.ModBase == scope_base);
            if (in_scope)
                return u64(buffer.info.Address);
        }
    }

    // Otherwise sweep for "*spec*" and demand exactly one distinct address. Enumerate module by
    // module: SymEnumSymbols with a base of 0 does not reach modules whose symbols are still
    // deferred (SYMOPT_DEFERRED_LOADS), so an unqualified sweep would silently find nothing.
    cc::vector<symbol_match> matches;
    enum_context ctx{&matches, &_modules};
    auto mask = cc::format("*{}*", spec.symbol);

    for (auto const& module : _modules.all())
    {
        if (scope_base != 0 && module.base != scope_base)
            continue;

        SymEnumSymbols(_process, module.base, mask.c_str_materialize(), collect_symbol, &ctx);
    }

    dedupe_by_address(matches);

    if (matches.empty())
        return cc::error(symbol_error{cc::format("no symbol matching '{}'", spec.to_string()), {}});

    if (matches.size() > 1)
        return cc::error(symbol_error{cc::format("symbol '{}' is ambiguous", spec.to_string()), cc::move(matches)});

    return matches[0].address;
}

cc::vector<stack_frame> symbol_session::walk_stack(void* thread_handle, void const* context) const
{
    // StackWalk64 mutates the context it walks.
    CONTEXT ctx = *static_cast<CONTEXT const*>(context);

    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    // Bounded: a corrupt or hand-rolled frame chain must not spin forever.
    constexpr int max_frames = 64;

    cc::vector<stack_frame> frames;
    for (int i = 0; i < max_frames; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, _process, thread_handle, &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;

        if (frame.AddrPC.Offset == 0)
            break;

        stack_frame out;
        out.rip = frame.AddrPC.Offset;

        if (auto const sym = symbol_at(out.rip); sym.has_value())
            out.symbol = sym.value().name;
        if (auto const* m = _modules.find_by_address(out.rip))
            out.module = m->name;
        if (auto const line = line_at(out.rip); line.has_value())
        {
            out.file = line.value().file;
            out.line = line.value().line;
        }

        frames.push_back(cc::move(out));
    }

    return frames;
}
} // namespace itrace
