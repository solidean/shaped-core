# Cancelling an operation you started

`cnet::cancel_token`, and why it is not the deadline.
Back to [_index.md](_index.md), or the [readme](../readme.md).

## The two ways an operation ends early

A **deadline** ends it because the world was slow.
Every operation that touches the network carries a finite one by default, because "no timeout" is how a half-open
connection becomes a hung program.

A **token** ends it because the program changed its mind: a server shutting down, a user pressing stop, a speculative
connection that lost its race.
Happy eyeballs is the case that makes it unavoidable — it starts two connects and must abandon the loser, and no
deadline can express that.

## A token groups, a deadline bounds

One token covers the resolve, the connect, the handshake and every read of a single request.
Cancelling the request cancels all of them, and each of those still carries its own deadline.

They are two parameters rather than one budget on purpose.
For a plain connect they are the same grouping; above it they are not, and an HTTP client wants a per-read timeout
*inside* a request-wide cancel.
Fusing them would force one lifetime onto both.

```cpp
auto const token = cnet::cancel_token::create();

auto connecting = cnet::tcp_connect(io, where, cnet::deadline::after_secs(10), {}, token);
auto reading = conn->receive(buffer, cnet::deadline::after_secs(30), token);

token.cancel();   // both end as cancelled, whatever each one was waiting for
```

The default token is empty, allocates nothing and can never be cancelled — which is what the vast majority of calls,
the ones that never cancel, pass.

## What cancel promises, and what it does not

It does **not** stop anything now.
The socket may be mid-write, and nothing can un-send a byte.

The promise is the reactor's own: the operation completes with `cancelled` at the next opportunity, and no later work
happens on its behalf.
A connection is not closed by cancelling a read on it — the operation ends, the connection stays usable.

A token, once cancelled, stays cancelled.
An operation started with it afterwards fails at once, without reaching the reactor at all, because starting work
nobody wants is worse than answering immediately.

## How callers see it

As `cc::async_error::is_cancelled()`, not as an error carrying a code.

```cpp
auto const* const failure = reading->try_error();
if (failure != nullptr && failure->is_cancelled())
    ...
```

That is where cancellation already had a home in `cc::async`, and the alternative is worse in both directions: a
cancelled read reporting zero bytes would be indistinguishable from a peer that hung up, and a cancellation spelled as
an ordinary error would answer `is_cancelled()` with false.
The cost is the message, and a cancellation is the one failure whose reason the caller already knows.

## The race, and why it is not one

A cancel and a completion can happen at the same time.

The token's control block is what they meet in.
An operation registers with it *after* it has been submitted — a cancel arriving in between would otherwise be posted
ahead of the operation it means to cancel, so `attach` re-reads the token afterwards and cancels the operation itself
if it has to.
An operation deregisters as the first thing its completion handler does, so a cancel racing that completion finds a
registration that is still alive rather than an operation that has already freed itself.

`io_system::cancel` is called while the block's lock is held.
It only posts a message and wakes the reactor, so it cannot block on the reactor thread, and the lock order is
one-way: a cancel takes the block and then the mailbox, while a completing operation takes the block and has released
it again before touching anything else.
