#pragma once

#include "fwd.hh"
#include "json.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>

/// The LSP structures in use, named after the specification's in snake_case, each with `read` and `write_fields`.
/// A subset by design: a type is added here when a message needs it.
/// Generating the whole set from the specification's `metaModel.json` is the path once that becomes a chore ([docs/lsp.md](../../../../docs/lsp.md)).

/// How positions count characters within a line, negotiated in `initialize`.
enum class lsp::position_encoding : lsp::u8
{
    utf16,
    utf8,
};

/// 0-based line and character, the character in the negotiated encoding's units.
struct lsp::position
{
    i32 line = 0;
    i32 character = 0;

    constexpr bool operator==(position const&) const = default;
};

struct lsp::range
{
    position start;
    position end;

    constexpr bool operator==(range const&) const = default;
};

struct lsp::location
{
    cc::string uri;
    lsp::range range;
};

struct lsp::text_edit
{
    lsp::range range;
    cc::string new_text;
};

struct lsp::diagnostic_related_information
{
    lsp::location location;
    cc::string message;
};

enum class lsp::diagnostic_severity : lsp::i32
{
    error = 1,
    warning = 2,
    information = 3,
    hint = 4,
};

enum class lsp::diagnostic_tag : lsp::i32
{
    unnecessary = 1,
    deprecated = 2,
};

enum class lsp::message_type : lsp::i32
{
    error = 1,
    warning = 2,
    info = 3,
    log = 4,
    debug = 5,
};

enum class lsp::inlay_hint_kind : lsp::i32
{
    type = 1,
    parameter = 2,
};

namespace lsp
{

/// JSON-RPC and LSP error codes.
namespace error_code
{
inline constexpr i32 parse_error = -32700;
inline constexpr i32 invalid_request = -32600;
inline constexpr i32 method_not_found = -32601;
inline constexpr i32 invalid_params = -32602;
inline constexpr i32 internal_error = -32603;
inline constexpr i32 server_not_initialized = -32002;
inline constexpr i32 request_cancelled = -32800;
inline constexpr i32 content_modified = -32801;
} // namespace error_code
} // namespace lsp

struct lsp::diagnostic
{
    lsp::range range;
    diagnostic_severity severity = diagnostic_severity::error;
    cc::string code;
    cc::string source;
    cc::string message;
    cc::vector<diagnostic_tag> tags;
    cc::vector<diagnostic_related_information> related_information;
};

/// What a request answers when it does not answer with a result.
struct lsp::response_error
{
    i32 code = error_code::internal_error;
    cc::string message;
};

struct lsp::publish_diagnostics_params
{
    cc::string uri;
    cc::optional<i32> version;
    cc::vector<diagnostic> diagnostics;
};

struct lsp::text_document_identifier
{
    cc::string uri;
};

/// The params of a request about one document and nothing else, such as `textDocument/semanticTokens/full`.
struct lsp::text_document_params
{
    cc::string uri;
};

struct lsp::text_document_item
{
    cc::string uri;
    cc::string language_id;
    i32 version = 0;
    cc::string text;
};

/// One change of `didChange`: a range and its replacement, or the whole text when `range` is absent.
struct lsp::text_document_content_change
{
    cc::optional<lsp::range> range;
    cc::string text;
};

struct lsp::did_open_params
{
    text_document_item text_document;
};

struct lsp::did_change_params
{
    cc::string uri;
    i32 version = 0;
    cc::vector<text_document_content_change> content_changes;
};

struct lsp::did_close_params
{
    cc::string uri;
};

struct lsp::semantic_tokens_legend
{
    cc::vector<cc::string> token_types;
    cc::vector<cc::string> token_modifiers;
};

/// The relative five-integer encoding: delta line, delta start, length, type, modifier bits, per token.
struct lsp::semantic_tokens
{
    cc::vector<u32> data;
};

struct lsp::inlay_hint_params
{
    cc::string uri;
    lsp::range range;
};

struct lsp::inlay_hint
{
    lsp::position position;
    cc::string label;
    inlay_hint_kind kind = inlay_hint_kind::type;
    bool padding_left = false;
    bool padding_right = false;
    cc::vector<text_edit> text_edits;
};

struct lsp::log_message_params
{
    message_type type = message_type::log;
    cc::string message;
};

namespace lsp
{

// reading
[[nodiscard]] bool read(json::ref in, position& out);
[[nodiscard]] bool read(json::ref in, range& out);
[[nodiscard]] bool read(json::ref in, text_document_identifier& out);
[[nodiscard]] bool read(json::ref in, text_document_params& out);
[[nodiscard]] bool read(json::ref in, text_document_item& out);
[[nodiscard]] bool read(json::ref in, text_document_content_change& out);
[[nodiscard]] bool read(json::ref in, did_open_params& out);
[[nodiscard]] bool read(json::ref in, did_change_params& out);
[[nodiscard]] bool read(json::ref in, did_close_params& out);
[[nodiscard]] bool read(json::ref in, inlay_hint_params& out);

// writing
void write_fields(json::writer& w, position const& v);
void write_fields(json::writer& w, range const& v);
void write_fields(json::writer& w, location const& v);
void write_fields(json::writer& w, text_edit const& v);
void write_fields(json::writer& w, diagnostic_related_information const& v);
void write_fields(json::writer& w, diagnostic const& v);
void write_fields(json::writer& w, publish_diagnostics_params const& v);
void write_fields(json::writer& w, semantic_tokens_legend const& v);
void write_fields(json::writer& w, semantic_tokens const& v);
void write_fields(json::writer& w, inlay_hint const& v);
void write_fields(json::writer& w, log_message_params const& v);
} // namespace lsp
