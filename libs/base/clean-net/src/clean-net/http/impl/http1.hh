#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-net/http/message.hh>

/// HTTP/1.1 on the wire: one parser, two serializers, and no I/O at all.
///
/// Keeping the bytes and the connection apart is what makes this the piece that gets tested hardest.
/// It is also the only component in this library that parses bytes from outside the process, which is why it is
/// strict about everything it is allowed to be strict about.
///
/// **One parser for both directions.**
/// A request and a response differ in their first line and in how a body with no framing is read; everything else --
/// the header rules, the chunked encoding, the limits, the backpressure -- is the same, and a second copy of it
/// would be a second place for the two to disagree.
///
/// **HTTP/1.1 only, on purpose.**
/// HPACK, stream multiplexing and flow control are a different order of magnitude, and wanting HTTP/2 is a reason to
/// select a system backend rather than to write one.

namespace cnet::impl
{
/// What a peer is not allowed to make us allocate.
///
/// Every one of these is a defence against a message rather than a preference: a peer that sends headers forever is
/// indistinguishable from one that is slow, until something says stop.
struct http1_limits
{
    isize max_start_line_bytes = 8 * 1024;

    /// All headers together, not each.
    isize max_header_bytes = 64 * 1024;

    isize max_header_count = 100;
};

/// The head of a request, as it arrived.
struct http1_request_head
{
    http_method method = http_method::get;

    /// The request target exactly as written -- still percent-encoded, query included.
    cc::string target;

    http_headers headers;

    bool http_1_0 = false;
};

/// Turn a request into the bytes that go out.
///
/// **This is the security boundary for header injection**, and the only one: a newline in a header name or value
/// would end the head and start something the caller did not write, so every byte is checked here rather than
/// wherever the header was set.
/// One check, on the way to the wire, with no way around it.
///
/// `Host` is added from the target when the caller did not set one, since a request without it is not HTTP/1.1.
[[nodiscard]] cc::result<cc::string, error> write_request_head(http_request const& request, bool keep_alive);

/// Turn a response head into the bytes that go out, with the same checks in the same place.
///
/// `Content-Length` is written from `body_bytes` unless the caller already set a framing header, because a response
/// whose length nobody stated is one the connection has to be closed to end.
[[nodiscard]] cc::result<cc::string, error> write_response_head(i32 status,
                                                                cc::string_view reason,
                                                                http_headers const& headers,
                                                                i64 body_bytes,
                                                                bool keep_alive);

/// The reason phrase servers conventionally use for a status.
/// Servers are free to make one up and HTTP/2 has none at all, so nothing should ever branch on it.
[[nodiscard]] cc::string_view default_reason_phrase(i32 status);

/// Reads a message as it arrives, in whichever direction.
///
/// Fed whatever bytes turned up; it tells you how many it took.
/// Body bytes go straight to the sink, and a sink that takes fewer than it was offered stops the parser at exactly
/// that point.
/// Carrying that the rest of the way -- not reading more until the sink asks -- belongs to whoever owns the
/// connection; `cnet::resume_body` is how the client does it.
class http1_parser
{
public:
    /// Begin a response to `request_method`, which is what decides whether a body is expected at all.
    void start_response(http_method request_method, http1_limits const& limits = {});

    /// Begin a request, which is the same machine reading the other direction.
    void start_request(http1_limits const& limits = {});

    /// Feed bytes, and report how many were consumed.
    ///
    /// Consuming less than it was given means either the head is complete and the caller should look at it, or the
    /// sink pushed back.
    /// Whatever is left over must be offered again.
    [[nodiscard]] cc::result<isize, error> feed(cc::span<byte const> input,
                                                cc::function_ref<isize(cc::span<byte const> chunk)> sink);

    /// Tell the parser the peer closed the connection.
    ///
    /// A response with no `Content-Length` and no chunking ends exactly there, which is the only framing HTTP/1.0
    /// had -- so this is a completion for one shape of message and a truncation for every other.
    [[nodiscard]] cc::result<cc::unit, error> notify_end_of_stream();

    [[nodiscard]] bool head_complete() const { return _head_complete; }
    [[nodiscard]] bool message_complete() const { return _state == state::complete; }

    /// Valid once the head is complete, and only for the direction that was started.
    [[nodiscard]] http_response_head const& response() const { return _response; }
    [[nodiscard]] http_response_head& response() { return _response; }
    [[nodiscard]] http1_request_head const& request() const { return _request; }

    /// Whether this connection can be used again once the message is done.
    ///
    /// False for `Connection: close`, for HTTP/1.0 without `Connection: keep-alive`, and for a body that is
    /// delimited by the close itself -- in which case reuse is not a choice anybody has.
    [[nodiscard]] bool can_reuse_connection() const;

    /// How many body bytes have gone to the sink.
    [[nodiscard]] i64 body_bytes() const { return _body_bytes; }

private:
    enum class direction : u8
    {
        response,
        request,
    };

    enum class state : u8
    {
        start_line,
        header_line,
        body_to_length,
        body_chunk_size,
        body_chunk_data,
        body_chunk_data_crlf,
        trailer_line,
        body_to_close,
        complete,
    };

    /// How the body is framed, decided once the head is in.
    enum class framing : u8
    {
        none,
        content_length,
        chunked,
        until_close,
    };

    void reset(direction d, http1_limits const& limits);

    [[nodiscard]] cc::result<bool, error> take_line(cc::span<byte const> input, isize& cursor);
    [[nodiscard]] cc::result<cc::unit, error> parse_status_line(cc::string_view line);
    [[nodiscard]] cc::result<cc::unit, error> parse_request_line(cc::string_view line);
    [[nodiscard]] cc::result<cc::unit, error> parse_header_line(cc::string_view line);
    [[nodiscard]] cc::result<cc::unit, error> finish_head();
    [[nodiscard]] cc::result<cc::unit, error> parse_chunk_size(cc::string_view line);

    [[nodiscard]] http_headers& headers()
    {
        return _direction == direction::response ? _response.headers : _request.headers;
    }
    [[nodiscard]] http_headers const& headers() const
    {
        return _direction == direction::response ? _response.headers : _request.headers;
    }

    direction _direction = direction::response;
    state _state = state::start_line;
    framing _framing = framing::none;
    http1_limits _limits;

    http_method _request_method = http_method::get;
    http_response_head _response;
    http1_request_head _request;
    bool _head_complete = false;

    /// The line being read, without its terminator.
    cc::vector<byte> _line;
    isize _header_bytes = 0;

    i64 _remaining = 0;
    i64 _body_bytes = 0;

    bool _http_1_0 = false;
    bool _connection_close = false;
};
} // namespace cnet::impl
