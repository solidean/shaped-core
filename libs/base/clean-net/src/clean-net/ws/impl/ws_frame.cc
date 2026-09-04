#include "ws_frame.hh"

#include <clean-core/string/format.hh>

// The frame format, and the rules that come with it.
//
// WHAT THE STRICTNESS IS FOR.
// A WebSocket connection is a byte stream both ends already agreed to trust, so the framing is not a security
// boundary the way the HTTP parser is.
// It is still worth being exact about, because every one of these rules exists to keep two implementations reading
// the same bytes the same way -- and a frame that one end reads as a 4 GB payload and the other as a control frame
// is a hang rather than an error message.

#ifndef CNET_HAS_TLS
#define CNET_HAS_TLS 0
#endif

#if CNET_HAS_TLS
#include <clean-net/tls/impl/mbedtls_threading.hh>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha1.h>
#endif

namespace cnet::impl
{
namespace
{
[[nodiscard]] error protocol(cc::string_view what)
{
    return {.code = error_code::protocol_error, .native_code = 0, .message = cc::string(what)};
}

/// The most a control frame may carry, so that answering one never needs a buffer.
constexpr i64 k_max_control_payload = 125;

/// The constant RFC 6455 appends to the key before hashing.
/// It is a magic string in the literal sense: it means nothing, and both ends must use exactly it.
constexpr cc::string_view k_accept_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
} // namespace

bool is_control_opcode(ws_opcode opcode)
{
    return (u8(opcode) & 0x8) != 0;
}

cc::result<cc::optional<ws_frame_header>, error> read_frame_header(cc::span<byte const> input)
{
    if (input.size() < 2)
        return cc::optional<ws_frame_header>();

    auto const first = u8(input[0]);
    auto const second = u8(input[1]);

    // The reserved bits are for extensions nobody here negotiated, so a peer setting one is talking to somebody else.
    if ((first & 0x70) != 0)
        return cc::error(protocol("a websocket frame with a reserved bit set"));

    auto header = ws_frame_header();
    header.fin = (first & 0x80) != 0;

    auto const opcode = u8(first & 0x0F);
    switch (opcode)
    {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x8:
    case 0x9:
    case 0xA:
        header.opcode = ws_opcode(opcode);
        break;
    default:
        return cc::error(protocol("a websocket frame with an opcode nobody defined"));
    }

    header.masked = (second & 0x80) != 0;
    auto const short_length = i64(second & 0x7F);

    auto cursor = isize(2);

    if (short_length <= 125)
    {
        header.payload_length = short_length;
    }
    else if (short_length == 126)
    {
        if (input.size() < cursor + 2)
            return cc::optional<ws_frame_header>();

        header.payload_length = (i64(u8(input[cursor])) << 8) | i64(u8(input[cursor + 1]));
        cursor += 2;

        // A length that fits in the shorter encoding must use it: two ways to write one number is two ways for two
        // implementations to disagree about what they read.
        if (header.payload_length < 126)
            return cc::error(protocol("a websocket length encoded in more bytes than it needed"));
    }
    else
    {
        if (input.size() < cursor + 8)
            return cc::optional<ws_frame_header>();

        header.payload_length = 0;
        for (isize i = 0; i < 8; ++i)
            header.payload_length = (header.payload_length << 8) | i64(u8(input[cursor + i]));
        cursor += 8;

        // The top bit must be clear: the length is signed in the RFC's own terms, and a negative one is nonsense
        // that only ever arrives on purpose.
        if (header.payload_length < 0)
            return cc::error(protocol("a websocket frame claiming a negative length"));
        if (header.payload_length <= 0xFFFF)
            return cc::error(protocol("a websocket length encoded in more bytes than it needed"));
    }

    if (header.masked)
    {
        if (input.size() < cursor + 4)
            return cc::optional<ws_frame_header>();

        for (isize i = 0; i < 4; ++i)
            header.mask[i] = u8(input[cursor + i]);
        cursor += 4;
    }

    if (is_control_opcode(header.opcode))
    {
        // Both of these are what make a control frame answerable without buffering: it arrives whole, and it is
        // small enough to hold on the stack.
        if (!header.fin)
            return cc::error(protocol("a fragmented websocket control frame"));
        if (header.payload_length > k_max_control_payload)
            return cc::error(protocol("a websocket control frame larger than 125 bytes"));
    }

    header.header_size = cursor;
    return cc::optional<ws_frame_header>(header);
}

void write_frame(cc::vector<byte>& out, ws_opcode opcode, cc::span<byte const> payload, bool mask, u8 const mask_key[4])
{
    out.push_back(byte(0x80 | u8(opcode))); // always FIN: nothing here fragments what it sends

    auto const length = payload.size();
    auto const mask_bit = u8(mask ? 0x80 : 0x00);

    if (length <= 125)
    {
        out.push_back(byte(mask_bit | u8(length)));
    }
    else if (length <= 0xFFFF)
    {
        out.push_back(byte(mask_bit | 126));
        out.push_back(byte(u8((length >> 8) & 0xFF)));
        out.push_back(byte(u8(length & 0xFF)));
    }
    else
    {
        out.push_back(byte(mask_bit | 127));
        for (isize i = 7; i >= 0; --i)
            out.push_back(byte(u8((length >> (i * 8)) & 0xFF)));
    }

    if (!mask)
    {
        for (auto const b : payload)
            out.push_back(b);
        return;
    }

    for (isize i = 0; i < 4; ++i)
        out.push_back(byte(mask_key[i]));

    for (isize i = 0; i < length; ++i)
        out.push_back(byte(u8(payload[i]) ^ mask_key[i % 4]));
}

void unmask(cc::span<byte> payload, u8 const mask_key[4], i64 offset)
{
    for (isize i = 0; i < payload.size(); ++i)
        payload[i] = byte(u8(payload[i]) ^ mask_key[(offset + i) % 4]);
}

bool is_valid_close_code(u16 code)
{
    // 1000-1003 and 1007-1011 are the ones an endpoint may send.
    // 1004 was never assigned; 1005 and 1006 exist only to be reported locally and must never appear on the wire;
    // 1015 is the same for a TLS failure.
    if (code >= 1000 && code <= 1003)
        return true;
    if (code >= 1007 && code <= 1011)
        return true;

    // 3000-3999 is registered per application, 4000-4999 is private use.
    return code >= 3000 && code <= 4999;
}

#if CNET_HAS_TLS

cc::result<cc::string, error> websocket_accept_key(cc::string_view client_key)
{
    ensure_mbedtls_threading();

    auto combined = cc::string(client_key);
    combined += k_accept_guid;

    unsigned char digest[20] = {};
    if (mbedtls_sha1(reinterpret_cast<unsigned char const*>(combined.data()), size_t(combined.size()), digest) != 0)
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the websocket accept key could not be hashed")});

    unsigned char encoded[64] = {};
    size_t written = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &written, digest, sizeof(digest)) != 0)
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the websocket accept key could not be encoded")});

    return cc::string(cc::string_view(reinterpret_cast<char const*>(encoded), isize(written)));
}

cc::result<cc::string, error> generate_websocket_key()
{
    ensure_mbedtls_threading();

    auto entropy = mbedtls_entropy_context();
    auto drbg = mbedtls_ctr_drbg_context();
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    struct scope_guard
    {
        mbedtls_entropy_context* entropy;
        mbedtls_ctr_drbg_context* drbg;

        ~scope_guard()
        {
            mbedtls_ctr_drbg_free(drbg);
            mbedtls_entropy_free(entropy);
        }
    } const guard{&entropy, &drbg};

    auto const personalization = cc::string_view("cnet websocket key");
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              reinterpret_cast<unsigned char const*>(personalization.data()),
                              size_t(personalization.size()))
        != 0)
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the websocket key generator could not be seeded")});

    // Sixteen bytes, as the RFC says: the value is not a secret, and its only job is to be different every time so
    // that a cached response cannot pass for a handshake.
    unsigned char nonce[16] = {};
    if (mbedtls_ctr_drbg_random(&drbg, nonce, sizeof(nonce)) != 0)
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the websocket key could not be generated")});

    unsigned char encoded[64] = {};
    size_t written = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &written, nonce, sizeof(nonce)) != 0)
        return cc::error(error{.code = error_code::unknown,
                               .native_code = 0,
                               .message = cc::string("the websocket key could not be encoded")});

    return cc::string(cc::string_view(reinterpret_cast<char const*>(encoded), isize(written)));
}

/// The process-wide generator mask keys are drawn from.
///
/// One rather than one per connection or per frame: seeding costs entropy and microseconds, and drawing does not.
/// `mbedtls_ctr_drbg_random` takes the context's own mutex under MBEDTLS_THREADING_C, which this build enables, so
/// it is safe from the reactor thread and from a caller's.
struct mask_source
{
    mbedtls_entropy_context entropy = {};
    mbedtls_ctr_drbg_context drbg = {};
    bool seeded = false;

    mask_source()
    {
        ensure_mbedtls_threading();
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);

        auto const personalization = cc::string_view("cnet websocket mask");
        seeded = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                       reinterpret_cast<unsigned char const*>(personalization.data()),
                                       size_t(personalization.size()))
              == 0;
    }

    // Never destroyed: it outlives every connection that draws from it, and tearing it down at exit would race a
    // reactor thread still writing frames.
    ~mask_source() = delete;
};

bool random_mask_key(u8 (&mask)[4])
{
    static auto* const source = new mask_source();
    if (!source->seeded)
        return false;

    return mbedtls_ctr_drbg_random(&source->drbg, reinterpret_cast<unsigned char*>(mask), sizeof(mask)) == 0;
}

#else

cc::result<cc::string, error> websocket_accept_key(cc::string_view)
{
    // The handshake needs SHA-1 and base64, which arrive with the TLS backend; a build without one is a build where
    // the browser owns the WebSocket anyway.
    return cc::error(unsupported_here("the websocket handshake"));
}

cc::result<cc::string, error> generate_websocket_key()
{
    return cc::error(unsupported_here("the websocket handshake"));
}

bool random_mask_key(u8 (&)[4])
{
    // No DRBG here, and no client either: the browser owns the WebSocket on this platform.
    return false;
}

#endif // CNET_HAS_TLS
} // namespace cnet::impl
