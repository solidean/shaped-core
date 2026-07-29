#pragma once

#include <clean-core/common/macros.hh>

// =========================================================================================================
// Sanitized <Windows.h>
// =========================================================================================================
//
// The one sanctioned way to reach <Windows.h> in shaped-core. Safe to include on ANY platform: on
// Windows it pulls in <Windows.h> behind the usual sanitization; everywhere else it expands to nothing.
//
// Sanitization keeps the mess windows.h drags into the global namespace confined to one audited place:
//   - WIN32_LEAN_AND_MEAN drops the rarely-needed sub-APIs (winsock, GDI extras, ...), cutting parse time.
//   - NOMINMAX suppresses the min()/max() function-like macros that otherwise clobber std::min/std::max,
//     cc::min/cc::max, and tg's math.
//   - `byte` is renamed away, see below.
// The first two are defined only if a TU hasn't already set it, so an including TU that wants a different
// balance (e.g. it also needs winsock) can define WIN32_LEAN_AND_MEAN itself beforehand and win.
//
// -------------------------------------------------------------------------------------------------------
// The `byte` rename
// -------------------------------------------------------------------------------------------------------
//
// <rpcndr.h> declares `typedef unsigned char byte;` at GLOBAL scope, with no opt-out guard of its own.
// That collides with `cc::byte` wherever a `using namespace cc::primitive_defines;` reaches global scope —
// as an AMBIGUITY, not a redefinition, so it breaks at the first bare `byte` use rather than at the include.
// Bracketing the SDK headers in `#define byte win_byte_override` renames the typedef and every use of it
// inside those headers alike, which is why the rename is invisible to the SDK's own declarations.
//
// WIN32_LEAN_AND_MEAN already keeps <rpc.h> out of <Windows.h>, so the bracket here is usually inert; it
// earns its keep for a TU that opted out of LEAN_AND_MEAN above. The headers that really do drag <rpcndr.h>
// in are the COM-based SDK stacks (<d3d12.h>, <dxcapi.h>, <wrl/*>), and each of their gates repeats this
// bracket over its own includes — the brackets compose, because the rename is the same one every time.
//
// Bracket only C headers. A C++ standard header parsed under the macro would declare `std::win_byte_override`
// instead of `std::byte`; that is why every gate includes its clean-core/std headers BEFORE opening a bracket.

#ifdef CC_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define byte win_byte_override
#include <Windows.h>
#undef byte

#endif
