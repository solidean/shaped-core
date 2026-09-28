#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

/// Base of the exceptions our libraries throw, carrying a formatted message.
/// Deliberately not a std::exception, so declaring one pulls in no standard header.
/// A catch site that knows only clean-core still reports the message: an async frame's error and nexus's uncaught-exception failure both read it.
/// So a library exception derives from this rather than rolling its own message member, or those sites see only "a non-std::exception value".
struct cc::exception
{
public:
    explicit exception(cc::string message) : _message(cc::move(message)) {}
    virtual ~exception() = default;

    exception(exception const&) = default;
    exception(exception&&) = default;
    exception& operator=(exception const&) = default;
    exception& operator=(exception&&) = default;

    [[nodiscard]] cc::string_view message() const { return _message; }

private:
    cc::string _message;
};
