# cnet: cancelling an operation you started

**Status:** decided, not built.
The smallest remaining gap in the TCP API.

## What is missing

`cnet::io_system::cancel(io_operation*)` exists and works.
A caller holding a `cc::shared_async` has no way to reach the operation behind it, so it cannot use it.

Today a **deadline is the only way an operation ends early**.
That is enough for a request with a budget and not enough for a server shutting down, a user pressing stop, or a
speculative connection that lost a race — and the last of those is not hypothetical: happy eyeballs
([cnet-name-resolution.md](cnet-name-resolution.md)) races two connects and must abandon the loser.

The cheat sheet says so rather than hiding it, which is the right interim state but not a resting place.

## Why it was left out

The TCP layer was built to prove the reactor, and every test it needed could be written with deadlines.
Adding a handle type before knowing what the layers above wanted from it would have been guessing.

Now the answer is known: happy eyeballs needs it, and so does the dev server's shutdown.

## What has to be decided

**What the caller holds.**
`cc::async` already models cancellation on its failure channel — `cc::async_error::make_cancelled()` and
`is_cancelled()` — so the *outcome* has a home.
What has no home is the request.
Three shapes, in order of how much they cost:

1. A separate `cnet::operation_handle` returned beside the async, which the caller keeps if it might cancel.
   Cheap, and it makes the common case (never cancel) pay nothing.
   Ugly at the call site, because every operation now returns two things.
2. A cancellation token passed *in*, held by the caller, shared by any number of operations.
   One token cancels a whole request — resolve, connect, handshake, read — which is exactly the grouping a caller
   wants, and the same grouping the deadline already has.
3. Cancellation on the `cc::shared_async` itself, if clean-core grows it.
   The nicest call site and the largest change, and it belongs to clean-core rather than here.

**The race.**
Cancel and completion can happen at the same time, and the reactor already tolerates this: `reactor::cancel` on an
operation it has forgotten is a no-op.
What the *handle* must not do is dereference an operation that has already freed itself, which it will have, since an
operation owns itself until it completes.
So the handle needs a weak reference of some kind — a generation counter, or a shared control block the operation and
the handle both hold.

**What cancel promises.**
Not "the operation stops now": the socket may be mid-write.
The honest promise is the same one the reactor already keeps — the operation completes with
`error_code::cancelled` at the next opportunity, and no later work happens on its behalf.

## Recommendation

**Take the token (shape 2).**
One deadline already covers a whole operation rather than each step of it, and cancellation wants exactly the same
grouping.
A token also composes downward without changing any signature above: an HTTP request hands its token to the connect,
the handshake and every read, and cancelling the request cancels all of them.

Give the token a shared control block, so a cancel racing a completion touches only the block and never the
operation.

## Traps already known

- **An operation owns itself until it completes.**
  Nothing outside the reactor may hold a raw pointer to one across a completion.
- **`cancel` is only safe from another thread through the actor**, which is why `io_system::cancel` posts a message
  rather than touching the reactor.
  A token must go through the same path.
- Do not spell the cancelled outcome as a distinct success.
  `cc::async_error::is_cancelled()` is what callers branch on, and a cancelled read that reported zero bytes would be
  indistinguishable from a peer that hung up.
