#pragma once

#include <clean-core/record/log.hh>
#include <typed-geometry/fwd.hh>

// SC_CHECK_GEOMETRY_SPECIAL_CASES reaches C++ as TG_CHECK_SPECIAL_CASES_REQUESTED, normalized here to 0 or 1.
// Like SC_CHECK_WIDE_ARITH it is whole-build: an inline function compiled both ways in one program is an ODR violation.
#if defined(TG_CHECK_SPECIAL_CASES_REQUESTED)
#define TG_CHECK_SPECIAL_CASES 1
#else
#define TG_CHECK_SPECIAL_CASES 0
#endif

/// Marks an input a geometric query assumes away; `cond` is true exactly when the input IS that special case.
///
/// With TG_CHECK_SPECIAL_CASES it logs a warning in the `tg` domain, which nexus turns into a failure for a test that
/// did not declare it; otherwise `cond` is not evaluated at all.
/// The query still returns whatever its formula gives, so the check never changes a result.
/// Constant evaluation skips the log, so a query stays usable in a constant expression either way.
#if TG_CHECK_SPECIAL_CASES
#define TG_SPECIAL_CASE(cond, what)                                 \
    do                                                              \
    {                                                               \
        if !consteval                                               \
        {                                                           \
            if (cond)                                               \
                CC_LOG_WARNING("geometric special case: {}", what); \
        }                                                           \
    } while (false)
#else
#define TG_SPECIAL_CASE(cond, what) \
    do                              \
    {                               \
    } while (false)
#endif
