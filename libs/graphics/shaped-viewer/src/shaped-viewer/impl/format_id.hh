#pragma once

#include <clean-core/common/utility.hh> // cc::forward
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>

#include <type_traits>

namespace sv::impl
{
/// A formatted id, held just long enough for the call that hashes it.
///
/// Ids are formatted every frame, so the common short one must not allocate: it is rendered into the inline buffer,
/// and only an id longer than that falls back to a heap string.
/// `view()` points into whichever of the two holds it, so the value must outlive the view — which is why every caller
/// keeps it as a named local rather than formatting into a temporary.
class formatted_id
{
public:
    template <class... Args>
    explicit formatted_id(cc::format_string<std::type_identity_t<Args>...> fmt, Args&&... args)
    {
        _size = cc::format_to(_buffer, fmt, cc::forward<Args>(args)...);

        // format_to reports what the id WOULD have taken, so this is the truncation case rather than a fit.
        // Forwarding a second time is safe because formatting only reads its arguments, and it is necessary:
        // re-deducing them as lvalues would give `fmt` a different format_string type.
        if (_size > inline_capacity)
            _overflow = cc::format(fmt, cc::forward<Args>(args)...);
    }

    [[nodiscard]] cc::string_view view() const
    {
        return _overflow.empty() ? cc::string_view(_buffer, _size) : cc::string_view(_overflow);
    }

private:
    static constexpr isize inline_capacity = 128;

    char _buffer[inline_capacity] = {};
    isize _size = 0;
    cc::string _overflow;
};
} // namespace sv::impl
