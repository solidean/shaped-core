#include "wasm_frames.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/thread/atomic.hh>

using namespace cc::primitive_defines;

namespace
{
/// The marker every wasm frame carries, whatever the engine and whether or not the build kept its names.
constexpr cc::string_view wasm_function_marker = "wasm-function[";

[[nodiscard]] cc::string_view trimmed(cc::string_view s)
{
    while (!s.empty() && cc::is_space(s.front()))
        s = s.subview(1);
    while (!s.empty() && cc::is_space(s.back()))
        s = s.subview(cc::offset_size{.offset = 0, .size = s.size() - 1});
    return s;
}

[[nodiscard]] cc::string_view prefix_of(cc::string_view s, isize size)
{
    return s.subview(cc::offset_size{.offset = 0, .size = size});
}

/// Reads a run of decimal digits, leaving `s` past them.
/// Fails on overflow rather than wrapping: a wrong offset resolves to a confidently wrong function.
[[nodiscard]] bool read_u32(cc::string_view& s, u32& out)
{
    if (s.empty() || !cc::is_digit(s.front()))
        return false;

    u64 v = 0;
    while (!s.empty() && cc::is_digit(s.front()))
    {
        v = v * 10 + u64(s.front() - '0');
        if (v > 0xFFFF'FFFFull)
            return false;
        s = s.subview(1);
    }
    out = u32(v);
    return true;
}

/// Reads `0x` followed by hex digits, leaving `s` past them.
[[nodiscard]] bool read_hex_u32(cc::string_view& s, u32& out)
{
    if (s.size() < 3 || s.front() != '0' || (s[1] != 'x' && s[1] != 'X'))
        return false;
    s = s.subview(2);

    u64 v = 0;
    auto digits = 0;
    while (!s.empty())
    {
        auto const c = s.front();
        auto nibble = 0;
        if (cc::is_digit(c))
            nibble = c - '0';
        else if (c >= 'a' && c <= 'f')
            nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            nibble = c - 'A' + 10;
        else
            break;

        v = v * 16 + u64(nibble);
        if (v > 0xFFFF'FFFFull)
            return false;
        ++digits;
        s = s.subview(1);
    }

    if (digits == 0)
        return false;
    out = u32(v);
    return true;
}

/// Strips the module prefix V8 glues onto a wasm function's name.
///
/// V8 spells a named wasm frame `at mod.wasm.render (...)`, so the name arrives with the module in front of it.
/// Removed only when the prefix is the module this frame is actually in, so a function genuinely called
/// `something.render` keeps its name.
[[nodiscard]] cc::string_view strip_module_prefix(cc::string_view name, cc::string_view module)
{
    if (module.empty() || name.empty())
        return name;

    // The module as V8 spells it in the name is the URL's last path segment, up to the `-<hash>` the engine appends.
    auto tail = module;
    if (auto const slash = tail.rfind('/'); slash >= 0)
        tail = tail.subview(slash + 1);
    if (auto const dash = tail.rfind('-'); dash >= 0)
        tail = prefix_of(tail, dash);

    if (tail.empty() || !name.starts_with(tail))
        return name;

    auto const rest = name.subview(tail.size());
    if (rest.empty() || rest.front() != '.')
        return name;
    return rest.subview(1);
}

/// Splits `wasm://wasm/mod-0001957a:wasm-function[11]:0x4d1` into its module, index and offset.
/// Everything after the marker is fixed in shape, which is what makes this a parse rather than a search.
[[nodiscard]] bool parse_wasm_location(cc::string_view location, cc::impl::wasm_frame& frame)
{
    auto const marker = location.find(wasm_function_marker);
    if (marker < 0)
        return false;

    // The module is everything before the `:` that introduces the marker.
    auto module = prefix_of(location, marker);
    while (!module.empty() && module.back() == ':')
        module = prefix_of(module, module.size() - 1);
    frame.module = module;

    auto rest = location.subview(marker + wasm_function_marker.size());
    if (!read_u32(rest, frame.function_index))
        return false;
    if (rest.empty() || rest.front() != ']')
        return false;
    rest = rest.subview(1);
    if (rest.empty() || rest.front() != ':')
        return false;
    rest = rest.subview(1);

    // Older V8 spelled the second number as a decimal offset WITHIN the function, which names no place in the module.
    // Rejecting it is the point: it is a different quantity rather than a smaller version of this one.
    if (!read_hex_u32(rest, frame.code_offset))
        return false;
    if (!rest.empty())
        return false;

    frame.is_js = false;
    return true;
}

/// A JS frame's location, `file:line:column` — of which only the line is an address.
/// The column is parsed but discarded, since nothing downstream resolves against one.
[[nodiscard]] bool parse_js_location(cc::string_view location, cc::impl::wasm_frame& frame)
{
    // Scanning from the right, because a Windows path has a colon of its own and a URL has two.
    auto const last = location.rfind(':');
    if (last <= 0)
        return false;
    auto const second_last = prefix_of(location, last).rfind(':');
    if (second_last <= 0)
        return false;

    auto line = location.subview(cc::start_end{.start = second_last + 1, .end = last});
    auto column = location.subview(last + 1);

    u32 line_number = 0;
    u32 column_number = 0;
    if (!read_u32(line, line_number) || !line.empty())
        return false;
    if (!read_u32(column, column_number) || !column.empty())
        return false;

    frame.module = prefix_of(location, second_last);
    frame.code_offset = line_number;
    frame.function_index = 0;
    frame.is_js = true;
    return true;
}
} // namespace

cc::optional<cc::impl::wasm_frame> cc::impl::parse_wasm_frame(cc::string_view line)
{
    line = trimmed(line);
    if (line.empty())
        return {};

    auto frame = cc::impl::wasm_frame();
    auto name = cc::string_view();
    auto location = cc::string_view();

    if (line.starts_with("at "))
    {
        // V8: `at NAME (LOCATION)`, or `at LOCATION` where there is no name to give.
        auto const rest = line.subview(3);
        if (rest.ends_with(')'))
        {
            auto const open = rest.rfind('(');
            if (open < 0)
                return {};
            name = trimmed(prefix_of(rest, open));
            location = rest.subview(cc::start_end{.start = open + 1, .end = rest.size() - 1});
        }
        else
        {
            location = rest;
        }
    }
    else if (auto const at = line.find('@'); at >= 0)
    {
        // SpiderMonkey: `NAME@LOCATION`, with the name empty for an anonymous frame.
        name = prefix_of(line, at);
        location = line.subview(at + 1);
    }
    else
    {
        // The leading `Error` line, an elision marker, anything else the engine chose to write.
        return {};
    }

    location = trimmed(location);
    if (location.empty())
        return {};

    if (!parse_wasm_location(location, frame) && !parse_js_location(location, frame))
        return {};

    frame.name = frame.is_js ? name : strip_module_prefix(name, frame.module);
    return frame;
}

namespace
{
// The name table cc::symbolizer reads on wasm.
//
// Sized rather than grown: a capture may run inside a crash handler, where allocating is the failure this whole
// path exists to survive.
// A program has far fewer distinct frames than this; overflowing costs names for the frames past it and nothing else.
constexpr isize symbol_slots = 4096; // power of two, so the probe wraps with a mask
constexpr isize symbol_arena_bytes = 128 * 1024;

struct symbol_slot
{
    /// The captured address, or 0 for an empty slot.
    /// Written LAST and with release ordering, so a reader never sees a slot claimed before its name is there.
    cc::atomic<u32> address = 0;

    u32 name_offset = 0;
    u32 name_size = 0;
};

symbol_slot g_symbols[symbol_slots];
char g_symbol_arena[symbol_arena_bytes];
cc::atomic<isize> g_symbol_arena_used = 0;

/// Whoever holds this is mid-insert.
///
/// A try rather than a lock: a capture that cannot have it files nothing, which costs a name.
/// Waiting could cost the whole crash report, since the thread holding it may be the one that died.
cc::atomic<bool> g_symbol_insert_busy = false;

[[nodiscard]] isize slot_of(u32 address)
{
    // Fibonacci hashing, so consecutive code offsets do not land in consecutive slots.
    auto const mixed = u32(address * 2654435761u);
    return isize(mixed & u32(symbol_slots - 1));
}
} // namespace

void cc::impl::remember_wasm_symbol(u32 address, cc::string_view name)
{
    if (address == 0 || name.empty() || name.size() > 0xFFFF)
        return;

    // Already known, and the common case by far: a stack is captured over and over at the same sites.
    // Checked without the guard, since a filled slot never changes.
    for (auto probe = isize(0); probe < 8; ++probe)
    {
        auto const& slot = g_symbols[(slot_of(address) + probe) & (symbol_slots - 1)];
        auto const claimed = slot.address.load(cc::memory_order_acquire);
        if (claimed == address)
            return;
        if (claimed == 0)
            break;
    }

    if (g_symbol_insert_busy.exchange(true, cc::memory_order_acquire))
        return;

    auto const used = g_symbol_arena_used.load(cc::memory_order_relaxed);
    if (used + name.size() <= symbol_arena_bytes)
    {
        for (auto probe = isize(0); probe < 8; ++probe)
        {
            auto& slot = g_symbols[(slot_of(address) + probe) & (symbol_slots - 1)];
            auto const claimed = slot.address.load(cc::memory_order_relaxed);
            if (claimed == address)
                break;
            if (claimed == 0)
            {
                cc::memcpy(g_symbol_arena + used, name.data(), size_t(name.size()));
                slot.name_offset = u32(used);
                slot.name_size = u32(name.size());
                g_symbol_arena_used.store(used + name.size(), cc::memory_order_relaxed);

                // Last, so a concurrent reader sees the name whenever it sees the address.
                slot.address.store(address, cc::memory_order_release);
                break;
            }
        }
    }

    g_symbol_insert_busy.store(false, cc::memory_order_release);
}

cc::string_view cc::impl::wasm_symbol_for(u32 address)
{
    if (address == 0)
        return {};

    for (auto probe = isize(0); probe < 8; ++probe)
    {
        auto const& slot = g_symbols[(slot_of(address) + probe) & (symbol_slots - 1)];
        auto const claimed = slot.address.load(cc::memory_order_acquire);
        if (claimed == 0)
            return {};
        if (claimed == address)
            return cc::string_view(g_symbol_arena + slot.name_offset, isize(slot.name_size));
    }
    return {};
}
