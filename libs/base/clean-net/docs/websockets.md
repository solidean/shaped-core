# WebSockets

`cnet::websocket` is a **message** channel over a byte stream, which is the whole reason the protocol exists.
TCP has no message boundaries, a browser needs them, and everything below is bookkeeping to hide that gap.

## The shape

```cpp
auto connecting = cnet::websocket_connect(io, resolver, "wss://example.com/feed");
// ... once it is ready:
auto ws = connecting->value();

auto const sent = ws->send_text("hello");
auto message = ws->receive();      // one whole message, whatever frames it arrived in
ws->close();
```

The server half is a route on the loopback dev server:

```cpp
server->websocket_route("/feed", [&](cc::shared_ptr<cnet::websocket> ws, cnet::http_server_request const&)
                        { sockets.push_back(cc::move(ws)); });
```

**The handler must keep the WebSocket alive.**
The server holds no reference to it, so one that is dropped closes — which is the right behaviour for a route that
decides it does not want the connection after all, and a bug nobody has to debug twice once it is stated.

## It is a wrapper, like TLS

`websocket` takes a connection and speaks a protocol over it, so it works over a socket, over a `virtual_network` with
no socket in sight, and over a `simulated_transport` that drops and delays bytes.
Every test in `websocket-test.cc` runs both ends in one process over a virtual network, which is why none of them
needs a port or a second process.

## What this layer decides for you

**Control frames are answered here.**
A ping is ponged, a close is acknowledged and ends the connection, and a pong is dropped.
None of these is a decision worth making per application, and all of them are things a peer will hold against a
connection that skips them.

**Sends are serialized.**
Frames may not interleave on the wire, so each send is framed, queued, and written when the one before it finishes.
Two `send_text` calls from different places therefore cannot corrupt each other's message.

**Receives are one at a time**, and a second one while the first is outstanding is a caller error rather than a queue:
the second would take the message the first was promised.
A message that arrives while nobody is waiting is held, in order, and handed over by the next `receive`.

**A message that arrived before the connection ended is delivered before the close is reported.**
The bytes are here; the close is the next thing the caller hears about rather than instead of them.

## What is a protocol error rather than a workaround

The framing is strict, and every rule it enforces exists so two implementations read the same bytes the same way:

- A reserved bit set, or an opcode nobody defined.
- A control frame that is fragmented, or longer than 125 bytes.
  Both are what make a control frame answerable without buffering anything.
- A length encoded in more bytes than it needed — two ways to write one number is two ways to disagree about what was
  read.
- **A client that does not mask, or a server that does.**
  Masking is not a security feature; it exists because a client that could put attacker-chosen bytes on the wire
  unaltered could poison a transparent proxy's cache.
  Which side masks is fixed by the protocol, and either end getting it wrong is a peer speaking something else.
- A continuation with nothing to continue, or a new message before the last one finished.
- A message past `max_message_bytes`.
  Without it a peer can send fragments forever and never set the final bit, which is a memory limit reached by a
  message that never arrives.

## The handshake

An HTTP request that, if it works, stops being HTTP — so it does **not** go through `http_client`.
Everything the client does (pooling, redirects, retries) is built on being finished with a connection when a response
ends, and this needs the connection afterwards.

`ws://` and `wss://` are rewritten to `http://` and `https://` and handed to `http_target`, which already refuses
credentials in the authority, unrepresentable ports and relative references.
The two scheme pairs differ in name only.

The client checks the 101 for `Upgrade`, `Connection`, and a `Sec-WebSocket-Accept` matching the key it sent — the
last is what makes the response an answer to *this* request rather than a replay.
`Sec-WebSocket-Accept` is SHA-1 of the key and a fixed string; SHA-1 is doing no cryptographic work there at all,
which is why its being broken does not matter.

A subprotocol the server picks must be one the client offered; anything else is a protocol the caller has no code for.
The server side never selects one, which every client must accept.

**Bytes that arrive with the handshake are kept.**
A peer may put its first message in the same packet as the handshake's last byte, and an implementation that starts
reading from the socket loses it.

## The server-side upgrade

`websocket_route` is checked before the ordinary routes, so a path can be both: a WebSocket when the request asks to
upgrade, and an ordinary response when it does not.

A request that matches a WebSocket route and is *not* a well-formed upgrade gets a **400**, not a 404 — a client that
meant to upgrade learns nothing from being told the path does not exist.

The 101 is written by hand rather than through `write_response_head`, which would add a `Content-Length`: a 101 has no
body, and every byte after its blank line already belongs to the WebSocket.

`open_connections()` counts HTTP connections, so an upgraded one leaves that count when it stops being HTTP.

## Where the handshake needs TLS

`Sec-WebSocket-Accept` needs SHA-1 and base64, and both arrive with the Mbed TLS backend.
A build without one (wasm) cannot perform the handshake, and does not need to: a browser owns the WebSocket there,
which is the same backend story as HTTP.

## Not here yet

- **A browser backend**, where `WebSocket` is the platform's and not ours to write.
- **Fragmented sends.** Everything sent goes out as one frame with `FIN` set; the reader handles fragments because
  peers send them.
- **Compression** (`permessage-deflate`), and the extension negotiation that goes with it.
- **UTF-8 validation** on text messages.
  The protocol requires it of a strict endpoint; nothing here checks.
