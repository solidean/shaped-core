#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// =========================================================================================================
// URI parsing, percent-encoding and reference resolution (RFC 3986)
// =========================================================================================================
//
// This is string algebra, not networking: nothing here opens a connection or resolves a name.
// A glTF file referencing an external buffer needs it exactly as much as an HTTP client does.
//
// cc::uri_view is a parse over BORROWED text -- it stores offsets into the string it was parsed from and must not
// outlive it.
// cc::uri owns its text, and hands out the same view.
// That is the cc::string_view / cc::string relationship, and it is the reason both exist: parsing a thousand
// references out of a document to look at their schemes should not allocate a thousand times.
//
// PERCENT-ENCODING IS PER COMPONENT, which is where the bugs are.
// `/` is legal unescaped in a path and must be escaped in a path segment; `?` is legal in a query and not in a path.
// So there is no one percent_encode: the component says which set applies, and it is never defaulted.

/// Which grammar's escaping set applies, for percent_encode.
///
/// Every set leaves the unreserved characters (ALPHA / DIGIT / `-` / `.` / `_` / `~`) alone and escapes everything
/// outside its own additions.
enum class cc::uri_component
{
    /// A whole path: `/`, `:`, `@` and the sub-delimiters survive.
    path,

    /// ONE path segment: as `path`, but `/` is escaped, since a slash here would invent a segment boundary.
    path_segment,

    /// A query string: as `path`, plus `/` and `?`.
    /// Note this is NOT form encoding -- a literal `+` survives, and a space becomes `%20` rather than `+`.
    query,

    /// A fragment: the same set as `query`.
    fragment,

    /// The userinfo of an authority: sub-delimiters and `:` survive.
    userinfo,

    /// A registered host name: sub-delimiters survive, `:` does not.
    host,

    /// `application/x-www-form-urlencoded`: everything but unreserved is escaped, and a space becomes `+`.
    /// This is the HTML form rule rather than RFC 3986, which is why it is a component of its own.
    form,
};

namespace cc::impl
{
/// Where each component sits inside the parsed text.
///
/// Split out of uri_view so cc::uri can hold the offsets beside its own cc::string and rebuild the view on demand.
/// A view stored inside the owner would dangle the moment the owner moved, because cc::string stores short text
/// inline.
struct uri_parts
{
    /// Length of the scheme; 0 means this is a relative reference with no scheme at all.
    i32 scheme_end = 0;

    /// First character of the authority, or -1 when there is no `//`.
    i32 authority_begin = -1;

    /// Index of the `@` ending the userinfo, or -1 when there is none.
    i32 userinfo_end = -1;

    /// First character of the host; equals authority_begin when there is no userinfo.
    i32 host_begin = -1;

    /// First digit of the port, or -1 when no `:` followed the host.
    i32 port_begin = -1;

    /// First character of the path.
    /// The path is the one component that is always present, possibly empty.
    i32 path_begin = 0;

    /// First character of the query, or -1 when there is no `?`.
    i32 query_begin = -1;

    /// First character of the fragment, or -1 when there is no `#`.
    i32 fragment_begin = -1;
};
} // namespace cc::impl

/// One `name=value` of a query string, still percent-encoded.
///
/// Decoding is left to the caller because it allocates, and a caller looking for one parameter out of twenty should
/// not pay for the other nineteen.
struct cc::uri_query_parameter
{
    cc::string_view name;

    /// Empty both when the parameter was written `name=` and when it was written bare; `has_value` tells them apart.
    cc::string_view value;

    bool has_value = false;
};

/// A parsed URI reference over borrowed text.
///
/// **Never outlives the string it was parsed from.** Every accessor returns a view into that same text.
///
/// Components come back exactly as written -- still percent-encoded, still in the case the author used.
/// percent_decode and normalize are the two ways to change that, and both are explicit because both cost something.
struct cc::uri_view
{
    uri_view() = default;

    /// Parse a URI reference (RFC 3986 section 4.1): absolute or relative, with or without a fragment.
    ///
    /// Fails on a malformed percent-escape, a non-numeric port, a scheme that does not match the grammar, and any
    /// whitespace or control byte -- those are how one URI silently becomes two.
    /// Bytes above 0x7F are tolerated rather than rejected: they are not legal RFC 3986, and real documents carry them.
    /// It does NOT fail on an unknown scheme or an unreachable host, which are not syntax.
    [[nodiscard]] static cc::optional<uri_view> parse(cc::string_view text);

    /// The whole reference, as it was parsed.
    [[nodiscard]] cc::string_view text() const { return _text; }

    /// The scheme without its `:`, in the case it was written; empty for a relative reference.
    [[nodiscard]] cc::string_view scheme() const { return _text.subview({.offset = 0, .size = _parts.scheme_end}); }

    /// Whether a scheme is present -- the difference between a URI and a relative reference.
    [[nodiscard]] bool is_absolute() const { return _parts.scheme_end > 0; }

    /// Whether the reference carried a `//` authority.
    /// `mailto:a@b` has none, and neither does `../x`.
    [[nodiscard]] bool has_authority() const { return _parts.authority_begin >= 0; }

    /// Host, userinfo and port together, without the leading `//`; empty when there is no authority.
    [[nodiscard]] cc::string_view authority() const;

    /// Whether an `@` ended a userinfo.
    /// `//@h` has an empty one, `//h` has none.
    [[nodiscard]] bool has_userinfo() const { return _parts.userinfo_end >= 0; }

    /// The userinfo without its `@`, still percent-encoded.
    ///
    /// A password here is deprecated by RFC 3986 section 3.2.1 and is a credential in a string that gets logged.
    [[nodiscard]] cc::string_view userinfo() const;

    /// The host, still percent-encoded, brackets included for an IPv6 literal.
    [[nodiscard]] cc::string_view host() const;

    /// The port as written, or absent when no `:` followed the host.
    /// A trailing `:` with nothing after it parses as an empty port, which is `has_port()` true and `port()` absent.
    [[nodiscard]] bool has_port() const { return _parts.port_begin >= 0; }
    [[nodiscard]] cc::string_view port_text() const;
    [[nodiscard]] cc::optional<i32> port() const;

    /// The path, still percent-encoded.
    /// Always present, possibly empty.
    [[nodiscard]] cc::string_view path() const;

    /// Whether a `?` was present.
    /// `a?` has an empty query, `a` has none.
    [[nodiscard]] bool has_query() const { return _parts.query_begin >= 0; }

    /// The query without its `?`, still percent-encoded.
    [[nodiscard]] cc::string_view query() const;

    /// Whether a `#` was present.
    [[nodiscard]] bool has_fragment() const { return _parts.fragment_begin >= 0; }

    /// The fragment without its `#`, still percent-encoded.
    [[nodiscard]] cc::string_view fragment() const;

    /// The parts, for cc::uri and for a caller building a view over text it moved itself.
    [[nodiscard]] cc::impl::uri_parts const& parts() const { return _parts; }

    /// Build a view over `text` from parts obtained elsewhere; `text` must be byte-identical to what produced them.
    [[nodiscard]] static uri_view from_parts(cc::string_view text, cc::impl::uri_parts const& parts);

    [[nodiscard]] friend bool operator==(uri_view lhs, uri_view rhs) { return lhs._text == rhs._text; }
    [[nodiscard]] friend bool operator!=(uri_view lhs, uri_view rhs) { return !(lhs == rhs); }

private:
    cc::string_view _text;
    cc::impl::uri_parts _parts;
};

/// A parsed URI reference that owns its text.
///
/// One allocation, and it exposes the same view as cc::uri_view.
/// Convert with view(); the accessors below forward to it for the common case.
struct cc::uri
{
    uri() = default;

    /// Parse and own a copy of `text`; the same grammar as uri_view::parse.
    [[nodiscard]] static cc::optional<uri> parse(cc::string_view text);

    /// Take ownership of already-parsed text.
    [[nodiscard]] static uri from_parsed(cc::string text, cc::impl::uri_parts const& parts);

    /// A view over this object's text.
    /// **Does not outlive it**, and a moved-from uri invalidates every earlier view.
    [[nodiscard]] uri_view view() const { return uri_view::from_parts(_text, _parts); }
    operator uri_view() const { return view(); }

    [[nodiscard]] cc::string_view text() const { return _text; }
    [[nodiscard]] cc::string const& owned_text() const { return _text; }

    [[nodiscard]] cc::string_view scheme() const { return view().scheme(); }
    [[nodiscard]] bool is_absolute() const { return view().is_absolute(); }
    [[nodiscard]] bool has_authority() const { return view().has_authority(); }
    [[nodiscard]] cc::string_view authority() const { return view().authority(); }
    [[nodiscard]] cc::string_view userinfo() const { return view().userinfo(); }
    [[nodiscard]] cc::string_view host() const { return view().host(); }
    [[nodiscard]] cc::optional<i32> port() const { return view().port(); }
    [[nodiscard]] cc::string_view path() const { return view().path(); }
    [[nodiscard]] bool has_query() const { return view().has_query(); }
    [[nodiscard]] cc::string_view query() const { return view().query(); }
    [[nodiscard]] bool has_fragment() const { return view().has_fragment(); }
    [[nodiscard]] cc::string_view fragment() const { return view().fragment(); }

    /// Resolve a relative reference against this URI as the base (RFC 3986 section 5.2).
    ///
    /// The base must be absolute, since a relative base cannot say what the result is relative to.
    /// Fails when `reference` does not parse, or when this is not absolute.
    [[nodiscard]] cc::optional<uri> resolve(cc::string_view reference) const;

    /// Syntax-based normalization (RFC 3986 section 6.2.2): scheme and host lowercased, `.` and `..` removed from the
    /// path, percent-escapes uppercased, and escapes of unreserved characters decoded.
    ///
    /// It does NOT apply scheme-specific rules -- no default port is dropped, and no empty path becomes `/`, because
    /// both are true of http and false in general.
    [[nodiscard]] uri normalized() const;

    [[nodiscard]] friend bool operator==(uri const& lhs, uri const& rhs) { return lhs._text == rhs._text; }
    [[nodiscard]] friend bool operator!=(uri const& lhs, uri const& rhs) { return !(lhs == rhs); }

private:
    cc::string _text;
    cc::impl::uri_parts _parts;
};

namespace cc
{
/// Percent-encode `s` for `component`.
///
/// The component is never defaulted, because the sets genuinely differ and picking the wrong one is silent:
/// a `/` escaped inside a whole path breaks it, and a `/` left unescaped inside a segment invents a boundary.
[[nodiscard]] cc::string percent_encode(cc::string_view s, uri_component component);

/// Decode `%XX` escapes; absent when an escape is truncated or not hexadecimal.
///
/// Strict about the input and permissive about the output: a decoded byte may be anything, including a NUL or an
/// incomplete UTF-8 sequence, because that is what the encoder was given.
/// `+` is left alone -- it means a space only in form encoding, and percent_decode_form is that.
[[nodiscard]] cc::optional<cc::string> percent_decode(cc::string_view s);

/// percent_decode, plus the form rule that `+` is a space.
///
/// Use it on the parts of a query string, and never on a path: a `+` in a path is a literal plus.
[[nodiscard]] cc::optional<cc::string> percent_decode_form(cc::string_view s);

/// Split a query string on `&` into its parameters, still percent-encoded.
///
/// `;` is not a separator: it was allowed by an old HTML recommendation and is not by any current one.
/// An empty run between two `&` is skipped rather than reported as a nameless parameter.
[[nodiscard]] cc::vector<uri_query_parameter> parse_query_parameters(cc::string_view query);

/// The first parameter named `name` in `query`, comparing the name percent-encoded as written.
///
/// Allocation-free, which is the point: a route handler looking for one parameter should not build a vector.
/// Absent when no parameter has that name; present and empty when it was written `name` or `name=`.
[[nodiscard]] cc::optional<cc::string_view> find_query_parameter(cc::string_view query, cc::string_view name);

/// Remove `.` and `..` segments from a path (RFC 3986 section 5.2.4).
///
/// A `..` that would climb above the root is dropped, never propagated: a path can only be used to reach out of its
/// own tree if something later treats it as a file name, and this is the algorithm that stops it.
[[nodiscard]] cc::string remove_dot_segments(cc::string_view path);
} // namespace cc
