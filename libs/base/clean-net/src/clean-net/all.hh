#pragma once

// Everything clean-net exposes, for a caller who would rather include one header than five.
// It is also the umbrella a precompiled-header tier would name, once this library is big enough to measure one.

#include <clean-net/address/endpoint.hh>
#include <clean-net/address/ip_address.hh>
#include <clean-net/common/cancel.hh>
#include <clean-net/common/clock.hh>
#include <clean-net/common/deadline.hh>
#include <clean-net/common/error.hh>
#include <clean-net/common/level.hh>
#include <clean-net/fwd.hh>
