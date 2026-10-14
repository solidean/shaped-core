#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <shaped-graphics/fwd.hh>

/// What one live array declaration and the command list it was made on share.
/// The list clears `list` when it ends the declaration from its side — a rendering scope closing, a submit, the list dropped.
/// So the value reaches the list only while `list` is set.
struct sg::impl::array_declaration_link
{
    command_list* list = nullptr;
    u32 id = 0;

    /// Made on the raster bind point, so the rendering scope's end ends it too.
    bool raster = false;
};

/// One array binding's declared per-element access, in force on the bind point it was made on while this value lives.
/// Move-only: destroying or `withdraw`ing it ends the declaration, and a moved-from one ends nothing.
/// Submitting or dropping its list ends it silently.
/// A raster declaration also ends when its rendering scope closes.
/// Once ended by any of those, destroying the value does nothing — it never touches a list that is gone.
///
/// A declaration that never met its array asserts when it ends, if some op of its bind point had a bound group holding an array of its family.
/// That is where a misspelled name is caught, at a withdraw, a destruction, or the rendering scope's close that ends a raster one.
/// A bind point that ran no op, or bound only arrays of the other family, proves nothing about the name, and ends its declarations silently.
/// An end while an exception unwinds past the declaration skips the check, as a submit and a drop do.
/// See libs/graphics/shaped-graphics/docs/concepts/bindings.md#array-bindings.
class sg::array_access_declaration
{
public:
    array_access_declaration() = default;
    array_access_declaration(array_access_declaration&& rhs) noexcept = default;
    array_access_declaration& operator=(array_access_declaration&& rhs) noexcept;
    ~array_access_declaration();

    array_access_declaration(array_access_declaration const&) = delete;
    array_access_declaration& operator=(array_access_declaration const&) = delete;

    /// Ends the declaration now rather than at destruction.
    void withdraw();

private:
    friend class command_list;
    explicit array_access_declaration(impl::array_declaration_token token) : _token(cc::move(token)) {}

    // Shared with the list, which clears it when the declaration ends from its side — scope close, submit, drop.
    // The value reaches the list only through a live token.
    impl::array_declaration_token _token;
};
