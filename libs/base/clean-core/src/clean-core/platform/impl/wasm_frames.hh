#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

// Parsing one line of a JavaScript engine's stack trace into something a stack capture can use.
//
// A wasm frame has no return address in linear memory, so the only account of the call stack is the string a JS
// engine formats for an Error.
// Each of its lines names the module and — this is the part that matters — the **byte offset into the code
// section** where the call is, which is an address in every sense that matters: DWARF and source maps both resolve
// against it, and it survives stripping the name section.
//
// Two engine spellings, and they are handled together because a parser that knows one silently reports nothing on
// the other:
//
//   V8 / node / Chrome   `    at NAME (wasm://wasm/mod-0001957a:wasm-function[11]:0x4d1)`
//   SpiderMonkey         `    NAME@wasm://wasm/mod-0001957a:wasm-function[11]:0x4d1`
//
// A build stripped of its name section drops the NAME and the module's filename, leaving
// `    at wasm://wasm/000182ce:wasm-function[10]:0x49e` — measured, and the offsets are byte-identical to the
// named build's, which is what makes resolving a stripped build after the fact possible at all.
//
// **Parsing lives here rather than in the capture** so it can be tested on every platform.
// The strings are the contract with the engines, not with Emscripten, so a recorded sample of each is a better test
// than anything a wasm run could assert about itself.

// Whether this build reports a frame per call the source makes.
// Set by tools/cmake/Emscripten.cmake, where the same build types that keep the name section are decided; 1
// everywhere that is not a Release wasm build, since no other toolchain here collapses frames this way.
#ifndef CC_WASM_KEEPS_FRAME_STRUCTURE
#define CC_WASM_KEEPS_FRAME_STRUCTURE 1
#endif

namespace cc::impl
{
struct wasm_frame;

/// Parses one line of engine stack-trace text.
///
/// Returns nothing for a line that names no frame — the leading `Error`, a blank line, an engine's elision marker.
/// A JS frame parses like any other and is reported as one, since a wasm stack genuinely ends in the module loader
/// and hiding that would misreport who called us.
[[nodiscard]] cc::optional<wasm_frame> parse_wasm_frame(cc::string_view line);

/// Remembers what a captured address was called, so a symbolizer can answer for it later.
///
/// **This is the only route to a name in-process on wasm.**
/// A name exists solely in the engine's frame text, which exists solely at capture time — there is no module to
/// consult afterwards the way there is everywhere else.
/// So a capture files what it parsed, and cc::symbolizer reads the file.
///
/// The consequence is worth stating plainly: **in-process, only an address this process has captured can be
/// resolved.** Anything else needs the offline resolver, against the build's own sidecars.
///
/// Allocation-free and bounded, because a crash handler captures too: a fixed table of entries over a fixed arena,
/// and both full means names stop being recorded rather than anything failing.
/// Best-effort under contention as well — a capture that cannot take the table skips filing rather than waiting,
/// since a thread frozen holding it would hang the crash report.
void remember_wasm_symbol(u32 address, cc::string_view name);

/// What `address` was called when it was captured, or empty.
[[nodiscard]] cc::string_view wasm_symbol_for(u32 address);

/// The bit marking an address as a JS line number rather than a wasm code offset.
///
/// A wasm module cannot reach 2 GiB of code, so the top bit is free, and Emscripten's own helpers already use it
/// this way — worth matching, since a recording may be read by their tooling as well as ours.
inline constexpr u32 wasm_js_frame_bit = 0x8000'0000u;
} // namespace cc::impl

/// One frame of a JS engine's stack trace, as far as text can say.
///
/// `code_offset` is what a symbolizer resolves, and it is meaningful only against the module it came from — see
/// the build identity recorded beside a wasm stack.
struct cc::impl::wasm_frame
{
    /// Byte offset into the module's code section, or the source line for a JS frame.
    cc::u32 code_offset = 0;

    /// The module's function index, from `wasm-function[N]`; 0 for a JS frame.
    cc::u32 function_index = 0;

    /// A JS frame rather than a wasm one, so `code_offset` is a line number and `function_index` means nothing.
    bool is_js = false;

    /// The function's name, empty when the build kept no name section.
    /// A view into the line this was parsed from, so it lives exactly as long as that text does.
    cc::string_view name;

    /// The module or script the frame is in, empty when the engine reported none.
    /// A view into the line, as `name` is.
    cc::string_view module;

    /// The address a capture writes for this frame: the code offset, or the line number with `wasm_js_frame_bit` set.
    [[nodiscard]] cc::u32 address() const { return is_js ? (code_offset | wasm_js_frame_bit) : code_offset; }
};
