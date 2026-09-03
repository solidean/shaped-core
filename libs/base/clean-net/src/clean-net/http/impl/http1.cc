#include "http1.hh"

#include <clean-core/common/asserts.hh>
#include <clean-core/string/format.hh>

// The wire format, and nothing else.
//
// WHY IT IS STRICT.
// Almost every HTTP attack that is not a bug in an application is a disagreement about framing: two parties reading
// the same bytes as different numbers of messages.
// A parser that repairs what it does not understand is how one of those disagreements happens, so this one refuses
// instead -- bare LF line endings, a space before a header colon, an obs-fold continuation, and above all a message
// carrying both Content-Length and Transfer-Encoding.
//
// The cost is a response somebody's nonconforming server sends that we will not read.
// The alternative is being the party that reads it differently from everyone else, which is worse.

namespace cnet::impl
{
namespace
{
[[nodiscard]] error malformed(cc::string_view what)
{
    return {.code = error_code::protocol_error, .native_code = 0, .message = cc::string(what)};
}

[[nodiscard]] char lowered(char c)
{
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}

/// Whether the byte may appear in a header name.
///
/// The RFC 9110 token set.
/// A space is the one that matters: `Content-Length : 5` is read as a header by some parsers and as nonsense by
/// others, which is exactly the disagreement to avoid.
[[nodiscard]] bool is_token_char(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;

    switch (c)
    {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

/// Whether the byte may appear in a header value.
/// Visible characters, space and tab; everything else -- CR, LF and the other controls -- is refused.
[[nodiscard]] bool is_field_value_char(char c)
{
    auto const u = static_cast<unsigned char>(c);
    return u == '\t' || (u >= 0x20 && u != 0x7F);
}

[[nodiscard]] cc::string_view trimmed(cc::string_view text)
{
    auto start = isize(0);
    auto end = text.size();
    while (start < end && (text[start] == ' ' || text[start] == '\t'))
        ++start;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
    return text.subview({.start = start, .end = end});
}

[[nodiscard]] bool equals_ignoring_case(cc::string_view a, cc::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (isize i = 0; i < a.size(); ++i)
        if (lowered(a[i]) != lowered(b[i]))
            return false;
    return true;
}

/// Whether a comma-separated list contains a token, which is how Connection is written.
[[nodiscard]] bool list_contains_token(cc::string_view list, cc::string_view token)
{
    auto start = isize(0);
    while (start <= list.size())
    {
        auto end = start;
        while (end < list.size() && list[end] != ',')
            ++end;

        if (equals_ignoring_case(trimmed(list.subview({.start = start, .end = end})), token))
            return true;

        start = end + 1;
    }
    return false;
}

/// The declared body length, or nothing when it is absent or not a number.
[[nodiscard]] cc::optional<i64> content_length_of(http_headers const& headers)
{
    auto const value = headers.get("Content-Length");
    if (!value.has_value())
        return {};

    auto length = i64(0);
    auto digits = 0;
    for (auto const c : value.value())
    {
        if (c < '0' || c > '9')
            return {};

        length = length * 10 + (c - '0');
        ++digits;

        // A length longer than any body anybody can hold is a malformed header rather than a big file.
        if (digits > 18)
            return {};
    }

    if (digits == 0)
        return {};
    return length;
}

[[nodiscard]] cc::string_view as_text(cc::vector<byte> const& bytes)
{
    return cc::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size());
}
} // namespace

// ---- the serializer ------------------------------------------------------------------------------------

cc::result<cc::string, error> write_request_head(http_request const& request, bool keep_alive)
{
    auto const target = request.target.request_target();

    // The request line is built from a closed method set and a target that came out of a URL parser, so the only way
    // a control character reaches it is through the target -- checked here, since this is the last point before the
    // bytes leave.
    for (auto const c : cc::string_view(target))
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) == 0x7F)
            return cc::error(malformed("the request target contains a character that cannot be sent"));

    auto head = cc::format("{} {} HTTP/1.1\r\n", to_string(request.method), target);

    auto wrote_host = false;
    for (auto const& header : request.headers.entries())
    {
        if (header.name.empty())
            return cc::error(malformed("a header with no name cannot be sent"));

        for (auto const c : cc::string_view(header.name))
            if (!is_token_char(c))
                return cc::error(error{.code = error_code::invalid_argument,
                                       .native_code = 0,
                                       .message = cc::format("the header name {} contains a character that cannot be "
                                                             "sent",
                                                             header.name)});

        for (auto const c : cc::string_view(header.value))
            if (!is_field_value_char(c))
                return cc::error(error{
                    .code = error_code::invalid_argument,
                    .native_code = 0,
                    .message = cc::format("the value of {} contains a character that cannot be sent", header.name)});

        if (header_names_equal(header.name, "Host"))
            wrote_host = true;

        head += cc::format("{}: {}\r\n", header.name, header.value);
    }

    // A request without Host is not HTTP/1.1, and the target is where it comes from when the caller did not say.
    if (!wrote_host)
        head += cc::format("Host: {}\r\n", request.target.host_header());

    if (!keep_alive)
        head += "Connection: close\r\n";

    head += "\r\n";
    return head;
}

cc::string_view default_reason_phrase(i32 status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 413:
        return "Content Too Large";
    case 426:
        return "Upgrade Required";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        // Servers are free to make one up, and nothing may branch on it -- so a status this list does not name gets
        // a phrase that is honest about being a placeholder rather than a guess at what it means.
        return "Status";
    }
}

cc::result<cc::string, error> write_response_head(i32 status,
                                                  cc::string_view reason,
                                                  http_headers const& headers,
                                                  i64 body_bytes,
                                                  bool keep_alive)
{
    if (status < 100 || status > 599)
        return cc::error(malformed("a status code outside 100-599 cannot be sent"));

    auto const phrase = reason.empty() ? default_reason_phrase(status) : reason;
    for (auto const c : phrase)
        if (!is_field_value_char(c))
            return cc::error(malformed("the reason phrase contains a character that cannot be sent"));

    auto head = cc::format("HTTP/1.1 {} {}\r\n", status, phrase);

    auto wrote_framing = false;
    for (auto const& header : headers.entries())
    {
        if (header.name.empty())
            return cc::error(malformed("a header with no name cannot be sent"));

        for (auto const c : cc::string_view(header.name))
            if (!is_token_char(c))
                return cc::error(error{.code = error_code::invalid_argument,
                                       .native_code = 0,
                                       .message = cc::format("the header name {} contains a character that cannot be "
                                                             "sent",
                                                             header.name)});

        for (auto const c : cc::string_view(header.value))
            if (!is_field_value_char(c))
                return cc::error(error{
                    .code = error_code::invalid_argument,
                    .native_code = 0,
                    .message = cc::format("the value of {} contains a character that cannot be sent", header.name)});

        if (header_names_equal(header.name, "Content-Length") || header_names_equal(header.name, "Transfer-Encoding"))
            wrote_framing = true;

        head += cc::format("{}: {}\r\n", header.name, header.value);
    }

    // A response whose length nobody stated is one the connection has to be closed to end, which is a keep-alive
    // connection wasted and a client left guessing.
    if (!wrote_framing)
        head += cc::format("Content-Length: {}\r\n", body_bytes);

    if (!keep_alive)
        head += "Connection: close\r\n";

    head += "\r\n";
    return head;
}

// ---- the parser ----------------------------------------------------------------------------------------

void http1_parser::reset(direction d, http1_limits const& limits)
{
    _direction = d;
    _state = state::start_line;
    _framing = framing::none;
    _limits = limits;
    _response = {};
    _request = {};
    _head_complete = false;
    _line.clear();
    _header_bytes = 0;
    _remaining = 0;
    _body_bytes = 0;
    _http_1_0 = false;
    _connection_close = false;
}

void http1_parser::start_response(http_method request_method, http1_limits const& limits)
{
    reset(direction::response, limits);
    _request_method = request_method;
}

void http1_parser::start_request(http1_limits const& limits)
{
    reset(direction::request, limits);
}

cc::result<bool, error> http1_parser::take_line(cc::span<byte const> input, isize& cursor)
{
    auto const cap = _state == state::start_line ? _limits.max_start_line_bytes : _limits.max_header_bytes;

    while (cursor < input.size())
    {
        auto const c = char(input[cursor++]);
        if (c == '\n')
        {
            // CRLF and nothing else: a parser that also takes a bare LF disagrees with one that does not, and that
            // disagreement is the whole of request smuggling.
            if (_line.empty() || char(_line[_line.size() - 1]) != '\r')
                return cc::error(malformed("a line ended without a carriage return"));

            _line.remove_back();
            return true;
        }

        _line.push_back(byte(c));
        if (_line.size() > cap)
            return cc::error(malformed("a line was longer than this client will read"));
    }

    return false;
}

cc::result<cc::unit, error> http1_parser::parse_status_line(cc::string_view line)
{
    // HTTP-version SP status-code [ SP reason ]
    if (line.size() < 12 || line.subview({.offset = 0, .size = 7}) != "HTTP/1.")
        return cc::error(malformed("the response does not begin with an HTTP/1 status line"));

    if (line[7] == '0')
        _http_1_0 = true;
    else if (line[7] != '1')
        return cc::error(malformed("an HTTP minor version this client does not speak"));

    if (line[8] != ' ')
        return cc::error(malformed("no space after the HTTP version"));

    auto status = 0;
    for (isize i = 9; i < 12; ++i)
    {
        auto const c = line[i];
        if (c < '0' || c > '9')
            return cc::error(malformed("the status code is not three digits"));
        status = status * 10 + (c - '0');
    }

    if (status < 100 || status > 599)
        return cc::error(malformed("the status code is outside 100-599"));

    _response.status = status;

    if (line.size() > 13)
        _response.reason = cc::string(line.subview(13));
    else if (line.size() == 13)
        return cc::error(malformed("a trailing space with no reason phrase"));

    return cc::unit{};
}

/// The method as it appears on a request line, or nothing for one this server does not implement.
[[nodiscard]] cc::optional<http_method> method_from_text(cc::string_view text)
{
    if (text == "GET")
        return http_method::get;
    if (text == "HEAD")
        return http_method::head;
    if (text == "POST")
        return http_method::post;
    if (text == "PUT")
        return http_method::put;
    if (text == "DELETE")
        return http_method::del;
    if (text == "PATCH")
        return http_method::patch;
    if (text == "OPTIONS")
        return http_method::options;
    return {};
}

cc::result<cc::unit, error> http1_parser::parse_request_line(cc::string_view line)
{
    // method SP request-target SP HTTP-version
    auto const first_space = line.find(' ');
    if (first_space <= 0)
        return cc::error(malformed("a request line without a method"));

    auto const method = method_from_text(line.subview({.offset = 0, .size = first_space}));
    if (!method.has_value())
        return cc::error(malformed("a request method this server does not implement"));

    auto const rest = line.subview(first_space + 1);
    auto const second_space = rest.find(' ');
    if (second_space <= 0)
        return cc::error(malformed("a request line without a target"));

    auto const target = rest.subview({.offset = 0, .size = second_space});
    auto const version = rest.subview(second_space + 1);

    // The target is checked here rather than by whoever routes on it: a control character in one is how a request
    // line becomes two, and this is where the bytes stop being a stream.
    for (auto const c : target)
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) == 0x7F)
            return cc::error(malformed("a request target with a character that cannot appear in one"));

    if (version.size() != 8 || version.subview({.offset = 0, .size = 7}) != "HTTP/1.")
        return cc::error(malformed("a request that is not HTTP/1"));

    if (version[7] == '0')
        _http_1_0 = true;
    else if (version[7] != '1')
        return cc::error(malformed("an HTTP minor version this parser does not speak"));

    _request.method = method.value();
    _request.target = cc::string(target);
    _request.http_1_0 = _http_1_0;
    return cc::unit{};
}

cc::result<cc::unit, error> http1_parser::parse_header_line(cc::string_view line)
{
    // An obs-fold continuation: deprecated by RFC 9112, and the classic way two parsers disagree about where a
    // header ends.
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
        return cc::error(malformed("a folded header line is not accepted"));

    auto const colon = line.find(':');
    if (colon <= 0)
        return cc::error(malformed("a header line without a name"));

    auto const name = line.subview({.offset = 0, .size = colon});
    for (auto const c : name)
        if (!is_token_char(c))
            return cc::error(malformed("a header name with a character that is not allowed in one"));

    auto const value = trimmed(line.subview(colon + 1));
    for (auto const c : value)
        if (!is_field_value_char(c))
            return cc::error(malformed("a header value with a character that is not allowed in one"));

    if (headers().size() >= _limits.max_header_count)
        return cc::error(malformed("more headers than this parser will read"));

    headers().add(name, value);
    return cc::unit{};
}

cc::result<cc::unit, error> http1_parser::finish_head()
{
    _head_complete = true;

    auto const connection = headers().get("Connection");
    if (connection.has_value())
        _connection_close = list_contains_token(connection.value(), "close");
    else
        _connection_close = _http_1_0;

    if (_http_1_0 && connection.has_value() && list_contains_token(connection.value(), "keep-alive"))
        _connection_close = false;

    // Framing, in the order RFC 9112 gives -- and the order matters, because a message that satisfies two of these
    // rules at once is the one an attacker sends.
    auto const has_length = headers().contains("Content-Length");
    auto const transfer_encoding = headers().get("Transfer-Encoding");
    auto const chunked = transfer_encoding.has_value() && list_contains_token(transfer_encoding.value(), "chunked");

    if (transfer_encoding.has_value() && has_length)
        return cc::error(malformed("a message with both Content-Length and Transfer-Encoding"));

    if (headers().get_all("Content-Length").size() > 1)
        return cc::error(malformed("a message with more than one Content-Length"));

    // A 1xx, a 204 and a 304 have no body whatever they say, and neither does the answer to a HEAD.
    auto const bodyless = _direction == direction::response
                       && (_response.is_informational() || _response.status == 204 || _response.status == 304
                           || _request_method == http_method::head);

    if (bodyless)
    {
        _framing = framing::none;
        _state = state::complete;
        return cc::unit{};
    }

    if (chunked)
    {
        _framing = framing::chunked;
        _state = state::body_chunk_size;
        return cc::unit{};
    }

    if (transfer_encoding.has_value())
        return cc::error(malformed("a Transfer-Encoding this client does not implement"));

    if (has_length)
    {
        auto const length = content_length_of(headers());
        if (!length.has_value())
            return cc::error(malformed("a Content-Length that is not a number"));

        _framing = framing::content_length;
        _remaining = length.value();
        _state = _remaining > 0 ? state::body_to_length : state::complete;
        return cc::unit{};
    }

    // A request with no framing header has no body at all -- there is no "until close" on the way in, because a
    // server that waited for one would wait for a client that is waiting for it.
    if (_direction == direction::request)
    {
        _framing = framing::none;
        _state = state::complete;
        return cc::unit{};
    }

    // For a response, nothing saying how long it is means the close says: HTTP/1.0's only framing, and a connection
    // that cannot be reused by definition.
    _framing = framing::until_close;
    _state = state::body_to_close;
    return cc::unit{};
}

cc::result<cc::unit, error> http1_parser::parse_chunk_size(cc::string_view line)
{
    // A chunk extension after `;` is legal and carries nothing anybody uses.
    auto const semicolon = line.find(';');
    auto const digits = semicolon < 0 ? line : line.subview({.offset = 0, .size = semicolon});

    if (digits.empty() || digits.size() > 16)
        return cc::error(malformed("a chunk size that is not a hexadecimal number"));

    auto size = i64(0);
    for (auto const c : digits)
    {
        auto value = 0;
        if (c >= '0' && c <= '9')
            value = c - '0';
        else if (c >= 'a' && c <= 'f')
            value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            value = c - 'A' + 10;
        else
            return cc::error(malformed("a chunk size that is not a hexadecimal number"));

        size = size * 16 + value;
    }

    _remaining = size;
    _state = size == 0 ? state::trailer_line : state::body_chunk_data;
    return cc::unit{};
}

cc::result<isize, error> http1_parser::feed(cc::span<byte const> input,
                                            cc::function_ref<isize(cc::span<byte const> chunk)> sink)
{
    auto cursor = isize(0);

    while (cursor < input.size() && _state != state::complete)
    {
        switch (_state)
        {
        case state::start_line:
        {
            auto complete = take_line(input, cursor);
            if (complete.has_error())
                return cc::error(cc::move(complete).error());
            if (!complete.value())
                return cursor;

            auto parsed = _direction == direction::response ? parse_status_line(as_text(_line))
                                                            : parse_request_line(as_text(_line));
            _line.clear();
            if (parsed.has_error())
                return cc::error(cc::move(parsed).error());

            _state = state::header_line;
            break;
        }

        case state::header_line:
        {
            auto complete = take_line(input, cursor);
            if (complete.has_error())
                return cc::error(cc::move(complete).error());
            if (!complete.value())
                return cursor;

            _header_bytes += _line.size();
            if (_header_bytes > _limits.max_header_bytes)
                return cc::error(malformed("more header bytes than this client will read"));

            if (_line.empty())
            {
                if (auto finished = finish_head(); finished.has_error())
                    return cc::error(cc::move(finished).error());

                // The head is worth looking at before the body arrives, so this stops here rather than reading on.
                return cursor;
            }

            auto parsed = parse_header_line(as_text(_line));
            _line.clear();
            if (parsed.has_error())
                return cc::error(cc::move(parsed).error());
            break;
        }

        case state::body_to_length:
        case state::body_chunk_data:
        {
            auto const available = input.size() - cursor;
            auto const wanted = _remaining < available ? isize(_remaining) : available;

            auto const taken = sink(input.subspan({.offset = cursor, .size = wanted}));
            CC_ASSERT(taken >= 0 && taken <= wanted, "a body sink consumed a number of bytes it was never offered");

            cursor += taken;
            _remaining -= taken;
            _body_bytes += taken;

            if (_remaining == 0)
                _state = _state == state::body_to_length ? state::complete : state::body_chunk_data_crlf;

            // Backpressure: the sink took less than it could have, so nothing more happens until it is asked again.
            if (taken < wanted)
                return cursor;
            break;
        }

        case state::body_chunk_data_crlf:
        {
            auto complete = take_line(input, cursor);
            if (complete.has_error())
                return cc::error(cc::move(complete).error());
            if (!complete.value())
                return cursor;

            if (!_line.empty())
            {
                _line.clear();
                return cc::error(malformed("a chunk that did not end where its size said"));
            }
            _state = state::body_chunk_size;
            break;
        }

        case state::body_chunk_size:
        {
            auto complete = take_line(input, cursor);
            if (complete.has_error())
                return cc::error(cc::move(complete).error());
            if (!complete.value())
                return cursor;

            auto parsed = parse_chunk_size(as_text(_line));
            _line.clear();
            if (parsed.has_error())
                return cc::error(cc::move(parsed).error());
            break;
        }

        case state::trailer_line:
        {
            auto complete = take_line(input, cursor);
            if (complete.has_error())
                return cc::error(cc::move(complete).error());
            if (!complete.value())
                return cursor;

            auto const empty = _line.empty();

            // Trailers are parsed for their syntax and then dropped: nothing above this reads them yet, and letting
            // them into the head after a caller has already seen it would be a surprise rather than a feature.
            if (!empty)
            {
                _header_bytes += _line.size();
                if (_header_bytes > _limits.max_header_bytes)
                    return cc::error(malformed("more trailer bytes than this client will read"));

                if (auto const colon = as_text(_line).find(':'); colon <= 0)
                {
                    _line.clear();
                    return cc::error(malformed("a trailer line without a name"));
                }
            }

            _line.clear();
            if (empty)
                _state = state::complete;
            break;
        }

        case state::body_to_close:
        {
            auto const available = input.size() - cursor;
            auto const taken = sink(input.subspan({.offset = cursor, .size = available}));
            CC_ASSERT(taken >= 0 && taken <= available, "a body sink consumed a number of bytes it was never offered");

            cursor += taken;
            _body_bytes += taken;

            if (taken < available)
                return cursor;
            break;
        }

        case state::complete:
            break;
        }
    }

    return cursor;
}

cc::result<cc::unit, error> http1_parser::notify_end_of_stream()
{
    if (_state == state::complete)
        return cc::unit{};

    // The one message shape a close completes rather than truncates.
    if (_state == state::body_to_close)
    {
        _state = state::complete;
        return cc::unit{};
    }

    if (!_head_complete)
        return cc::error(malformed("the connection closed before the response head was complete"));

    return cc::error(malformed("the connection closed in the middle of the response body"));
}

bool http1_parser::can_reuse_connection() const
{
    if (_connection_close || _framing == framing::until_close)
        return false;
    return _state == state::complete;
}
} // namespace cnet::impl
