#pragma once

// The one header a library's own fwd.hh includes to give its namespace a recording domain.
//
// It declares nothing but an incomplete type and a constexpr address, so it costs a fwd.hh essentially nothing:
// no format machinery, no containers, not even <clean-core/fwd.hh>.
// The full domain object lives in clean-core/record/domain.hh, which only the .cc defining the domain needs.

namespace cc::rec
{
struct domain;

/// The domain a recording site falls back to when no enclosing namespace declares one of its own.
/// Defined in domain.cc; declared here so the global cc_rec_domain() below is a constant expression.
extern domain g_default_domain;
} // namespace cc::rec

/// Which domain the recording macros attribute a site to, resolved by ORDINARY UNQUALIFIED NAME LOOKUP.
///
/// This global overload is the fallback, and a library shadows it for its own code by declaring one inside its
/// namespace with CC_REC_DECLARE_DOMAIN.
/// Lookup from inside `sg::impl` then walks out to `sg`, finds sg's, and stops — so every site in the library is
/// attributed without naming a domain, and a site outside any of them lands here.
[[nodiscard]] constexpr ::cc::rec::domain* cc_rec_domain()
{
    return &::cc::rec::g_default_domain;
}

/// Declare `var_name` as the enclosing namespace's recording domain; put this in the library's fwd.hh.
///
///   namespace sg { CC_REC_DECLARE_DOMAIN(g_rec_domain); }
///
/// Exactly one .cc must then define it with CC_REC_DEFINE_DOMAIN, in the same namespace.
#define CC_REC_DECLARE_DOMAIN(var_name)                        \
    extern ::cc::rec::domain var_name;                         \
    [[nodiscard]] constexpr ::cc::rec::domain* cc_rec_domain() \
    {                                                          \
        return &var_name;                                      \
    }

/// Defining one is CC_REC_DEFINE_DOMAIN, over in clean-core/record/domain.hh — it needs the complete type, and the
/// .cc that defines a domain is already including the full machinery anyway.
