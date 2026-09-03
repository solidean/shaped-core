#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-net/http/message.hh>

/// HTTP/1.1 on the wire: one serializer, one parser, and no I/O at all.
///
/// Keeping the bytes and the connection apart is what makes this the piece that gets tested hardest.
/// It is also the only component in this library that parses bytes from outside the process, which is why it is
/// strict about everything it is allowed to be strict about.
///
/// **HTTP/1.1 only, on purpose.**
/// HPACK, stream multiplexing and flow control are a different order of magnitude, and wanting HTTP/2 is a reason to
/// select a system backend rather than to write one.

namespace cnet::impl
{
/// What a peer is not allowed to make us allocate.
///
/// Every one of these is a defence against a response rather than a preference: a server that sends headers forever
/// is indistinguishable from one that is slow, until something says stop.
struct http1_limits
{
    isize max_status_line_bytes = 8 * 1024;

    /// All headers together, not each.
    isize max_header_bytes = 64 * 1024;

    isize max_header_count = 100;
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

/// Reads a response as it arrives.
///
/// Fed whatever bytes turned up; it tells you how many it took.
/// Body bytes go straight to the sink, and a sink that takes fewer than it was offered stops the parser at exactly
/// that point -- which is how backpressure reaches all the way down to the socket.
class http1_response_parser
{
public:
    /// Begin a response to `request_method`, which is what decides whether a body is expected at all.
    void start(http_method request_method, http1_limits const& limits = {});

    /// Feed bytes, and report how many were consumed.
    ///
    /// Consuming less than it was given means either the head is complete and the caller should look at it, or the
    /// sink pushed back.
    /// Whatever is left over must be offered again.
    [[nodiscard]] cc::result<isize, error> feed(cc::span<byte const> input, body_sink sink);

    /// Tell the parser the peer closed the connection.
    ///
    /// A response with no `Content-Length` and no chunking ends exactly there, which is the only framing HTTP/1.0
    /// had -- so this is a completion for one shape of message and a truncation for every other.
    [[nodiscard]] cc::result<cc::unit, error> notify_end_of_stream();

    [[nodiscard]] bool head_complete() const { return _head_complete; }
    [[nodiscard]] bool message_complete() const { return _state == state::complete; }

    [[nodiscard]] http_response_head const& head() const { return _head; }
    [[nodiscard]] http_response_head& head() { return _head; }

    /// Whether this connection can be used again once the message is done.
    ///
    /// False for `Connection: close`, for HTTP/1.0 without `Connection: keep-alive`, and for a body that is
    /// delimited by the close itself -- in which case reuse is not a choice anybody has.
    [[nodiscard]] bool can_reuse_connection() const;

    /// How many body bytes have gone to the sink.
    [[nodiscard]] i64 body_bytes() const { return _body_bytes; }

private:
    enum class state : u8
    {
        status_line,
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

    [[nodiscard]] cc::result<bool, error> take_line(cc::span<byte const> input, isize& cursor);
    [[nodiscard]] cc::result<cc::unit, error> parse_status_line(cc::string_view line);
    [[nodiscard]] cc::result<cc::unit, error> parse_header_line(cc::string_view line);
    [[nodiscard]] cc::result<cc::unit, error> finish_head();
    [[nodiscard]] cc::result<cc::unit, error> parse_chunk_size(cc::string_view line);

    state _state = state::status_line;
    framing _framing = framing::none;
    http1_limits _limits;

    http_method _request_method = http_method::get;
    http_response_head _head;
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
