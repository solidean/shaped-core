#pragma once

#include <clean-core/fwd.hh>

#include <exception>
#include <stdexcept>

// The blessed route to the standard exception types.
//
// We return a cc::result rather than throwing, and that is still the rule — see docs/coding-guidelines.md.
// But a few situations are genuinely exceptional and do throw: a lost GPU device, a cc::result the caller
// chose to convert, an assert handler unwinding to a recovery point.
// Those need `std::exception` to catch on and a concrete type to throw, so somewhere has to include these
// two headers.
//
// This file is that somewhere.
// It adds nothing of its own — including it is the whole API — so that the blessing sits in one place
// instead of in every library's .shaped-lint.yml.
//
// It is deliberately a seam rather than a wrapper.
// cc::exception, the non-std base, lives in exception_base.hh and needs neither header.
// `<stdexcept>` is the heavier of the two and the first candidate to drop as throwers move onto it.
// Reaching the std types through here is what makes that change one file's problem rather than a sweep.
