#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

/// Aggregate forward declarations for clean-net.
///
/// This header is also the API index: every name the library exposes is declared here, with the one line that says
/// what it is.
/// The design behind it is [docs/structure.md](../../docs/structure.md).
///
/// The one thing to know before reading further: **the transport and the protocol clients are peers, not layers**.
/// A browser has no sockets and does have HTTP, so an HTTP client written over our own TCP could not exist there --
/// and wasm is a tier-1 platform.
/// So HTTP and WebSocket are interfaces with backends, the transport is one of those backends, and each piece
/// reports its own availability.

namespace cnet
{
// Pull in the shaped-core vocabulary types (i32, u8, isize, ...) so we write them bare inside cnet without leaking
// them into the global namespace.
using namespace cc::primitive_defines;
} // namespace cnet

// ---- diagnostics ---------------------------------------------------------------------------------------

namespace cnet
{
/// Why a networking call failed, for the cases worth branching on.
///
/// The distinction that carries weight is between "never here", "not in this build" and "not this time":
/// the first two are answered once at startup, the third is worth retrying.
enum class error_code : u8;

/// What a failed call reports: a code, the platform's own number, and a message.
struct error;

/// The domain every recording site in clean-net is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace cnet

// ---- time ----------------------------------------------------------------------------------------------

namespace cnet
{
/// How long an operation may take before it fails with `timed_out`.
struct deadline;

/// The monotonic time source the reactor, the timeouts and the rate limits all read.
///
/// A seam rather than a call to the OS, because a test that has to sleep to prove a timeout fires is both slow and
/// flaky.
class clock;

/// The process-wide clock over the real one.
[[nodiscard]] clock& system_clock();

/// A clock that only moves when a test moves it.
class manual_clock;
} // namespace cnet

// ---- addresses -----------------------------------------------------------------------------------------

namespace cnet
{
/// Which of the two address families, or neither.
enum class ip_family : u8;

/// One IPv4 or IPv6 address, plus the scope id an IPv6 link-local address needs to be usable.
struct ip_address;

/// An address and a port -- what a socket connects to or listens on.
struct endpoint;
} // namespace cnet

// ---- the reactor ---------------------------------------------------------------------------------------

namespace cnet
{
/// How an io_system is built: threaded or not, against which clock, and how long one wait may park.
struct io_system_description;

/// The reactor, and the thread it may or may not have.
///
/// There is no `poll()` on it: with threads it owns one, and without them it registers with clean-core's pump
/// registry, so whatever already drives `cc::thread_pump_all()` drives this too.
class io_system;
} // namespace cnet

// ---- capability ladder ---------------------------------------------------------------------------------

namespace cnet
{
/// How much of HTTP a backend can actually do, as a ladder rather than a bag of capability queries.
///
/// Code written against a level works on every backend at that level or above, which is the property a bag of
/// booleans cannot give you.
enum class http_level : u8;
} // namespace cnet
