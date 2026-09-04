#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-net/common/deadline.hh>
#include <clean-net/common/error.hh>
#include <clean-net/http/http_target.hh>

/// What an HTTP request and response are made of.
///
/// Nothing here touches a connection: these are the values a backend takes and returns, and they are the same values
/// whether the request goes out over our own transport or over a browser's `fetch`.

namespace cnet::impl
{
/// What a `cnet::resume_body` holds: the one thing a paused request needs to be told to carry on.
///
/// Opaque on purpose, and refcounted intrusively for the same reason `cnet::cancel_token`'s control block is: it
/// holds a `cc::mutex`, whose header reaches MSVC's <xutility> and the whole AVX-512 intrinsic surface behind it.
/// A `cc::shared_ptr` member would need the complete type at every copy and destruction, and would drag all of that
/// into every translation unit that includes this header.
struct body_gate;

void body_gate_retain(body_gate* g);
void body_gate_release(body_gate* g);
void body_gate_resume(body_gate* g);
} // namespace cnet::impl

/// The methods this client can send.
///
/// A closed set rather than a string, because a method with a space or a newline in it is a request smuggling
/// primitive and an enum cannot carry one.
enum class cnet::http_method : cnet::u8
{
    get,
    head,
    post,
    put,

    /// `DELETE` on the wire; the C++ spelling avoids the keyword.
    del,

    patch,
    options,
};

namespace cnet
{
/// The method as it goes on the request line.
[[nodiscard]] cc::string_view to_string(http_method method);

/// Whether a method may be retried on its own.
///
/// Idempotence is the property that decides it: sending GET twice is what the caller asked for, and sending POST
/// twice is a second order.
[[nodiscard]] bool is_idempotent(http_method method);
} // namespace cnet

/// One header, as written.
struct cnet::http_header
{
    cc::string name;
    cc::string value;
};

/// The headers of a request or a response, in order.
///
/// Ordered rather than a map, because HTTP allows a header to appear more than once and the order of those
/// repetitions is meaningful -- `Set-Cookie` above all.
/// Lookup is case-insensitive, because the wire is.
///
/// **Nothing is validated here.**
/// A name or value carrying a newline is refused where it would do damage -- when the request is serialized -- so
/// that there is exactly one check and no way around it.
class cnet::http_headers
{
public:
    /// Append, keeping any header of the same name that is already there.
    void add(cc::string_view name, cc::string_view value);

    /// Replace every header of this name with one.
    void set(cc::string_view name, cc::string_view value);

    /// Add only if no header of this name is present, which is how a default is applied over a caller's choice.
    void set_if_absent(cc::string_view name, cc::string_view value);

    void remove(cc::string_view name);

    [[nodiscard]] bool contains(cc::string_view name) const;

    /// The first value of this name, or nothing.
    [[nodiscard]] cc::optional<cc::string_view> get(cc::string_view name) const;

    /// Every value of this name, in order.
    [[nodiscard]] cc::vector<cc::string_view> get_all(cc::string_view name) const;

    [[nodiscard]] cc::span<http_header const> entries() const { return _entries; }
    [[nodiscard]] isize size() const { return _entries.size(); }
    [[nodiscard]] bool empty() const { return _entries.empty(); }

    void clear() { _entries.clear(); }

private:
    cc::vector<http_header> _entries;
};

namespace cnet
{
/// Whether two header names are the same, which is a case-insensitive question.
[[nodiscard]] bool header_names_equal(cc::string_view a, cc::string_view b);
} // namespace cnet

/// How a sink that is not ready for more says so, and asks to be offered the rest.
///
/// **Calling `resume()` is what restarts the transport.** A sink that takes fewer bytes than it was offered stops the
/// request reading: nothing further is pulled off the connection, so the receive window closes and the sender slows
/// down, which is backpressure reaching all the way to TCP rather than into a buffer of ours.
/// A request that is never resumed ends on its own deadline, since a stalled consumer is not a reason to wait
/// forever.
///
/// Copyable and safe from any thread; the bytes are re-offered on the reactor thread rather than on the caller's.
/// Calling it on a request that has already finished, or more than once, is harmless.
class cnet::resume_body
{
public:
    void resume() const { impl::body_gate_resume(_gate); }

    /// A flow nothing is behind, which every `resume()` on it ignores.
    resume_body() = default;

    /// **For the client layer**, which is the only thing that has a gate to hand over.
    explicit resume_body(impl::body_gate* gate) : _gate(gate) { impl::body_gate_retain(_gate); }

    resume_body(resume_body const& o) : _gate(o._gate) { impl::body_gate_retain(_gate); }
    resume_body(resume_body&& o) noexcept : _gate(o._gate) { o._gate = nullptr; }
    resume_body& operator=(resume_body const& o);
    resume_body& operator=(resume_body&& o) noexcept;
    ~resume_body() { impl::body_gate_release(_gate); }

private:
    impl::body_gate* _gate = nullptr;
};

namespace cnet
{

/// Where a response body is handed to, chunk by chunk.
///
/// **The return value is the backpressure**: a sink that consumes less than it was offered stops the request reading
/// more, and `flow.resume()` is what starts it again.
/// That is why this is the primitive and the buffered response is written over it -- an unbounded buffer on a
/// download of unknown size is a real failure rather than a theoretical one, and backpressure is the one part of this
/// that cannot be added afterwards.
/// A sink that always takes everything never needs `flow` and can ignore it.
///
/// **It runs on the reactor thread.** Do no work here; hand the bytes on.
///
/// Owning rather than a `cc::function_ref`, because a request outlives the call that started it: a reference to a
/// lambda the caller wrote at the call site would dangle before the first byte arrived.
/// The parser underneath takes a reference instead, since it is synchronous and cannot outlive anything.
using body_sink = cc::unique_function<isize(cc::span<byte const> chunk, resume_body const& flow)>;
} // namespace cnet

/// What a caller sends.
struct cnet::http_request
{
    http_method method = http_method::get;

    /// Where it goes.
    http_target target;

    http_headers headers;

    /// The bytes to send, held WITH their owner.
    ///
    /// A request outlives the call that started it, so a bare span here would be a lifetime the caller has to keep
    /// straight on their own -- and one that compiles, passes on loopback, and corrupts a body on a slow link.
    /// `cc::make_pinned_data(cc::move(bytes))` moves an owned buffer in without copying it, and
    /// `cc::pinned_data<byte const>::create_from_pin(span, nullptr)` is the escape for memory the caller knows
    /// outlives the request.
    ///
    /// Empty for a method that carries no body.
    cc::pinned_data<byte const> body;
};

/// The head of a response: everything but the bytes.
struct cnet::http_response_head
{
    /// 100-599 as it arrived; 0 only on a response that never parsed.
    i32 status = 0;

    /// The reason phrase, which servers are free to make up and HTTP/2 does not have at all.
    /// Never branch on it.
    cc::string reason;

    http_headers headers;

    [[nodiscard]] bool is_informational() const { return status >= 100 && status < 200; }
    [[nodiscard]] bool is_success() const { return status >= 200 && status < 300; }
    [[nodiscard]] bool is_redirect() const { return status >= 300 && status < 400; }
    [[nodiscard]] bool is_client_error() const { return status >= 400 && status < 500; }
    [[nodiscard]] bool is_server_error() const { return status >= 500 && status < 600; }

    /// The body length the head declares, or nothing when it is delimited some other way.
    [[nodiscard]] cc::optional<i64> content_length() const;
};

/// A response whose body was buffered.
struct cnet::http_response
{
    http_response_head head;
    cc::vector<byte> body;

    [[nodiscard]] i32 status() const { return head.status; }
    [[nodiscard]] bool is_success() const { return head.is_success(); }

    /// The body as text, without copying it.
    /// Meaningless for a body that is not text, and this does not check.
    [[nodiscard]] cc::string_view body_text() const
    {
        return cc::string_view(reinterpret_cast<char const*>(body.data()), body.size());
    }
};
