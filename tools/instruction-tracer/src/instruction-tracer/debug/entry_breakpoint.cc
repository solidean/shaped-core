#include "entry_breakpoint.hh"

#include <clean-core/platform/win32_sanitized.hh>

namespace itrace
{
namespace
{
constexpr u8 int3_opcode = 0xCC;

bool read_byte(void* process, u64 address, u8& out)
{
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<void const*>(address), &out, 1, &read) && read == 1;
}

bool write_byte(void* process, u64 address, u8 value)
{
    // Code pages are not writable; flip the protection for the single byte, then flush the icache.
    DWORD old_protect = 0;
    auto* const target = reinterpret_cast<void*>(address);
    if (!VirtualProtectEx(process, target, 1, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;

    SIZE_T written = 0;
    bool const ok = WriteProcessMemory(process, target, &value, 1, &written) && written == 1;

    DWORD ignored = 0;
    VirtualProtectEx(process, target, 1, old_protect, &ignored);
    FlushInstructionCache(process, target, 1);

    return ok;
}
} // namespace

entry_breakpoint::entry_breakpoint(void* process, u64 address) : _process(process), _address(address)
{
}

bool entry_breakpoint::arm()
{
    if (_armed)
        return true;

    if (!read_byte(_process, _address, _original))
        return false;

    if (!write_byte(_process, _address, int3_opcode))
        return false;

    _armed = true;
    return true;
}

bool entry_breakpoint::disarm()
{
    if (!_armed)
        return true;

    if (!write_byte(_process, _address, _original))
        return false;

    _armed = false;
    return true;
}
} // namespace itrace
