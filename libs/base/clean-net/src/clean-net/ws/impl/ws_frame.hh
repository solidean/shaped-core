#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-net/common/error.hh>

/// WebSocket framing (RFC 6455), and nothing that touches a connection.
///
/// The frame format is small and the rules around it are not, which is why this is its own file with its own tests:
/// masking, fragmentation, control frames that may interrupt a fragmented message but may not themselves be
/// fragmented, and a close code space with holes in it.
///
/// **Masking is not a security feature and must still be exact.**
/// It exists because a client that could put attacker-chosen bytes on the wire unaltered could poison a transparent
/// proxy's cache, so a client masks and a server does not -- and either one seeing the wrong thing is a protocol
/// error rather than something to work around.

namespace cnet::impl
{
/// What a frame carries.
enum class ws_opcode : u8
{
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

/// Whether this opcode is a control frame.
///
/// Control frames may arrive in the middle of a fragmented message, may never be fragmented themselves, and carry at
/// most 125 bytes -- which is what makes them answerable without buffering anything.
[[nodiscard]] bool is_control_opcode(ws_opcode opcode);

/// A frame's header, once it has all arrived.
struct ws_frame_header
{
    bool fin = true;
    ws_opcode opcode = ws_opcode::text;
    bool masked = false;

    /// The masking key, meaningful only when `masked`.
    u8 mask[4] = {};

    i64 payload_length = 0;

    /// How many bytes of the input the header itself took.
    isize header_size = 0;
};

/// Read a frame header, or report that not enough has arrived yet.
///
/// Fails on the things that are protocol errors rather than incomplete input: a reserved bit set, an unknown opcode,
/// a control frame that is fragmented or too long, and a length encoded in more bytes than it needed.
[[nodiscard]] cc::result<cc::optional<ws_frame_header>, error> read_frame_header(cc::span<byte const> input);

/// Append a whole frame -- header and payload -- to `out`.
///
/// `mask` is what a client passes and a server must not: the side that masks is fixed by the protocol, and getting
/// it wrong is the one framing mistake every implementation makes once.
void write_frame(cc::vector<byte>& out, ws_opcode opcode, cc::span<byte const> payload, bool mask, u8 const mask_key[4]);

/// Unmask `payload` in place, starting `offset` bytes into the frame's payload.
///
/// The offset matters because a payload may arrive in pieces, and the mask repeats every four bytes from the start
/// of the payload rather than from the start of whatever chunk turned up.
void unmask(cc::span<byte> payload, u8 const mask_key[4], i64 offset);

/// Whether a close code is one an endpoint may send.
///
/// The space has holes in it: some codes exist only to be reported locally and must never appear on the wire, and
/// everything below 3000 that is not named is reserved.
[[nodiscard]] bool is_valid_close_code(u16 code);

/// The `Sec-WebSocket-Accept` value for a `Sec-WebSocket-Key`.
///
/// SHA-1 of the key and a constant, base64-encoded.
/// It proves the peer understood the handshake rather than anything about security -- SHA-1 is doing no cryptographic
/// work here at all, which is why its being broken does not matter.
[[nodiscard]] cc::result<cc::string, error> websocket_accept_key(cc::string_view client_key);

/// A fresh `Sec-WebSocket-Key`: 16 random bytes, base64-encoded.
[[nodiscard]] cc::result<cc::string, error> generate_websocket_key();
} // namespace cnet::impl
