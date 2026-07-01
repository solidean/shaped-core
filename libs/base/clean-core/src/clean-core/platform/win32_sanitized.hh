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
// Each is defined only if a TU hasn't already set it, so an including TU that wants a different balance
// (e.g. it also needs winsock) can define WIN32_LEAN_AND_MEAN itself beforehand and win.

#ifdef CC_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#endif
