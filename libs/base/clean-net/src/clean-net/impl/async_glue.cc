#include "async_glue.hh"

namespace cnet
{
cc::async_error to_async_error(error e)
{
    // A cancellation is spelled as cc::async's own cancellation rather than as an error carrying our code, because
    // `cc::async_error::is_cancelled()` is what a caller branches on and it would answer false otherwise.
    // The message is what that costs, and a cancellation is the one failure whose reason the caller already knows.
    if (e.code == error_code::cancelled)
        return cc::async_error::make_cancelled();

    return cc::async_error::make_error(cc::any_error(cc::move(e)));
}
} // namespace cnet
