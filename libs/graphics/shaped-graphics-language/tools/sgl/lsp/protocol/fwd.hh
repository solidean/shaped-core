#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/record/domain_fwd.hh>

/// The Language Server Protocol, and nothing about any language.
///
/// This folder is its own static library, `sgl-lsp-protocol`, which links clean-core and babel-data and nothing else,
/// so an SGL include here fails to compile: it is what a shared LSP library would be extracted from.
/// Positions are UTF-16 or UTF-8 as the client negotiated; everything beneath this layer speaks UTF-8 byte offsets.

namespace lsp
{
using namespace cc::primitive_defines;

enum class position_encoding : u8;
enum class diagnostic_severity : i32;
enum class diagnostic_tag : i32;
enum class message_type : i32;
enum class inlay_hint_kind : i32;
struct position;
struct range;
struct location;
struct text_edit;
struct diagnostic_related_information;
struct diagnostic;
struct response_error;
struct publish_diagnostics_params;
struct text_document_identifier;
struct text_document_params;
struct text_document_item;
struct text_document_content_change;
struct did_open_params;
struct did_change_params;
struct did_close_params;
struct semantic_tokens_legend;
struct semantic_tokens;
struct inlay_hint_params;
struct inlay_hint;
struct log_message_params;

class text_index;
struct document;
class snapshot;
class workspace;

struct cancel_flag;
struct request_context;
class server;

class frame_reader;
class log_listener;
class stdio_host;

/// The domain every recording site of the protocol layer is attributed to.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace lsp

namespace lsp::json
{
struct raw;
struct null_t;
} // namespace lsp::json
