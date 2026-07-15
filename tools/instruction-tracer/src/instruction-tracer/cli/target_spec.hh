#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <instruction-tracer/debug/trace_record.hh>

namespace itrace
{
/// Where to set the entry breakpoint.
struct target_spec
{
    enum class kind
    {
        symbol,        // "foo::bar" — searched across every loaded module
        address,       // "0x7ff611203410" — absolute runtime address
        module_symbol, // "mymodule.exe!foo::bar"
        module_offset, // "mymodule.exe+0x3410"
    };

    kind form = kind::symbol;
    cc::string module; // empty unless module_symbol / module_offset
    cc::string symbol; // empty unless symbol / module_symbol
    u64 address = 0;   // absolute for kind::address, module-relative for kind::module_offset

    /// How the spec was written, for diagnostics.
    cc::string to_string() const;
};

/// Parse a --target value; the form is inferred from the separators. `0x`-prefixed => address,
/// `mod!sym` => module_symbol, `mod+0xN` => module_offset, anything else => a bare symbol.
/// Fails on an empty spec or a malformed hex offset.
cc::result<target_spec> parse_target_spec(cc::string_view spec);

/// Parse an absolute address, with or without a `0x` prefix. Fails on empty/garbage/overflow.
cc::result<u64> parse_address(cc::string_view text);
} // namespace itrace
