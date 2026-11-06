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

/// What an editor draws beside a completion, in the specification's numbering; a subset.
enum class lsp::completion_item_kind : lsp::i32
{
    method = 2,
    function = 3,
    field = 5,
    variable = 6,
    property = 10,
    enum_ = 13,
    keyword = 14,
    enum_member = 20,
    constant = 21,
    struct_ = 22,
    type_parameter = 25,
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
/// The request was valid and the server declines it, as a rename to a keyword.
inline constexpr i32 request_failed = -32803;
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

struct lsp::code_action_params
{
    cc::string uri;
    lsp::range range;
};

/// A quick fix: the edits of one document that resolve `diagnostics`.
struct lsp::code_action
{
    cc::string title;
    cc::vector<diagnostic> diagnostics;
    bool is_preferred = false;
    /// The document `edits` change.
    cc::string uri;
    cc::vector<text_edit> edits;
};

/// The params of a request about one position of one document: hover, definition, completion.
struct lsp::text_document_position_params
{
    cc::string uri;
    lsp::position position;
};

/// A hover's contents, which are markdown.
struct lsp::hover
{
    cc::string markdown;
    cc::optional<lsp::range> range;
};

struct lsp::completion_item
{
    cc::string label;
    completion_item_kind kind = completion_item_kind::variable;
    cc::string detail;
};

/// One signature of a call; each parameter label is a substring of `label`, as the specification allows.
struct lsp::signature_information
{
    cc::string label;
    /// Markdown; empty for none.
    cc::string documentation;
    cc::vector<cc::string> parameters;
};

struct lsp::signature_help
{
    cc::vector<signature_information> signatures;
    i32 active_signature = 0;
    i32 active_parameter = 0;
};

struct lsp::reference_params
{
    cc::string uri;
    lsp::position position;
    bool include_declaration = true;
};

struct lsp::rename_params
{
    cc::string uri;
    lsp::position position;
    cc::string new_name;
};

/// The edits of one document within a `workspace_edit`.
struct lsp::document_edits
{
    cc::string uri;
    cc::vector<text_edit> edits;
};

struct lsp::workspace_edit
{
    cc::vector<document_edits> changes;
};

/// What `textDocument/prepareRename` answers: the name's range, and the name an editor offers to replace.
struct lsp::prepare_rename_result
{
    lsp::range range;
    cc::string placeholder;
};

/// What an editor draws beside a symbol, in the specification's numbering; a subset.
enum class lsp::symbol_kind : lsp::i32
{
    module = 2,
    namespace_ = 3,
    method = 6,
    property = 7,
    field = 8,
    enum_ = 10,
    function = 12,
    variable = 13,
    constant = 14,
    object = 19,
    enum_member = 22,
    struct_ = 23,
    event = 24,
    type_parameter = 26,
};

/// One declaration of a document's outline, with the ones it holds.
struct lsp::document_symbol
{
    cc::string name;
    cc::string detail;
    symbol_kind kind = symbol_kind::function;
    lsp::range range;
    lsp::range selection_range;
    cc::vector<document_symbol> children;
};

struct lsp::folding_range
{
    i32 start_line = 0;
    i32 end_line = 0;
    bool is_comment = false;
};

struct lsp::workspace_symbol_params
{
    cc::string query;
};

/// A declaration somewhere in the workspace, flat, with the declaration that holds it.
struct lsp::symbol_information
{
    cc::string name;
    symbol_kind kind = symbol_kind::function;
    lsp::location location;
    cc::string container_name;
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
[[nodiscard]] bool read(json::ref in, code_action_params& out);
[[nodiscard]] bool read(json::ref in, text_document_position_params& out);
[[nodiscard]] bool read(json::ref in, reference_params& out);
[[nodiscard]] bool read(json::ref in, rename_params& out);
[[nodiscard]] bool read(json::ref in, workspace_symbol_params& out);

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
void write_fields(json::writer& w, code_action const& v);
void write_fields(json::writer& w, log_message_params const& v);
void write_fields(json::writer& w, hover const& v);
void write_fields(json::writer& w, completion_item const& v);
void write_fields(json::writer& w, signature_information const& v);
void write_fields(json::writer& w, signature_help const& v);
void write_fields(json::writer& w, workspace_edit const& v);
void write_fields(json::writer& w, prepare_rename_result const& v);
void write_fields(json::writer& w, document_symbol const& v);
void write_fields(json::writer& w, folding_range const& v);
void write_fields(json::writer& w, symbol_information const& v);
} // namespace lsp
