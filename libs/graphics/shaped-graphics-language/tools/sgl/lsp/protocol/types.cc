#include "types.hh"

using namespace cc::primitive_defines;

bool lsp::read(json::ref in, position& out)
{
    return json::read(in["line"], out.line) && json::read(in["character"], out.character);
}

bool lsp::read(json::ref in, range& out)
{
    return read(in["start"], out.start) && read(in["end"], out.end);
}

bool lsp::read(json::ref in, text_document_identifier& out)
{
    return json::read(in["uri"], out.uri);
}

bool lsp::read(json::ref in, text_document_params& out)
{
    return json::read(in["textDocument"]["uri"], out.uri);
}

bool lsp::read(json::ref in, text_document_item& out)
{
    return json::read(in["uri"], out.uri) && json::read(in["languageId"], out.language_id)
        && json::read(in["version"], out.version) && json::read(in["text"], out.text);
}

bool lsp::read(json::ref in, text_document_content_change& out)
{
    // an absent or null range is a change of the whole text
    return json::read(in["text"], out.text) && json::read_optional(in["range"], out.range);
}

bool lsp::read(json::ref in, did_open_params& out)
{
    return read(in["textDocument"], out.text_document);
}

bool lsp::read(json::ref in, did_change_params& out)
{
    auto const doc = in["textDocument"];
    return json::read(doc["uri"], out.uri) && json::read(doc["version"], out.version)
        && json::read(in["contentChanges"], out.content_changes);
}

bool lsp::read(json::ref in, did_close_params& out)
{
    return json::read(in["textDocument"]["uri"], out.uri);
}

bool lsp::read(json::ref in, inlay_hint_params& out)
{
    return json::read(in["textDocument"]["uri"], out.uri) && read(in["range"], out.range);
}

bool lsp::read(json::ref in, code_action_params& out)
{
    return json::read(in["textDocument"]["uri"], out.uri) && read(in["range"], out.range);
}

bool lsp::read(json::ref in, text_document_position_params& out)
{
    return json::read(in["textDocument"]["uri"], out.uri) && read(in["position"], out.position);
}

bool lsp::read(json::ref in, reference_params& out)
{
    // `context` is required by the specification; a client that leaves it out asks for the declaration too
    if (auto const include = in["context"]["includeDeclaration"];
        include.is_valid() && !json::read(include, out.include_declaration))
        return false;
    return json::read(in["textDocument"]["uri"], out.uri) && read(in["position"], out.position);
}

bool lsp::read(json::ref in, rename_params& out)
{
    return json::read(in["textDocument"]["uri"], out.uri) && read(in["position"], out.position)
        && json::read(in["newName"], out.new_name);
}

bool lsp::read(json::ref in, workspace_symbol_params& out)
{
    return json::read(in["query"], out.query);
}

void lsp::write_fields(json::writer& w, position const& v)
{
    w.write("line", v.line);
    w.write("character", v.character);
}

void lsp::write_fields(json::writer& w, range const& v)
{
    json::put(w, "start", v.start);
    json::put(w, "end", v.end);
}

void lsp::write_fields(json::writer& w, location const& v)
{
    json::put(w, "uri", v.uri);
    json::put(w, "range", v.range);
}

void lsp::write_fields(json::writer& w, text_edit const& v)
{
    json::put(w, "range", v.range);
    json::put(w, "newText", v.new_text);
}

void lsp::write_fields(json::writer& w, diagnostic_related_information const& v)
{
    json::put(w, "location", v.location);
    json::put(w, "message", v.message);
}

void lsp::write_fields(json::writer& w, diagnostic const& v)
{
    json::put(w, "range", v.range);
    w.write("severity", i32(v.severity));
    if (!v.code.empty())
        json::put(w, "code", v.code);
    if (!v.source.empty())
        json::put(w, "source", v.source);
    json::put(w, "message", v.message);
    if (!v.tags.empty())
    {
        w.begin_array("tags");
        for (auto const t : v.tags)
            w.write(i32(t));
        w.end_array();
    }
    if (!v.related_information.empty())
        json::put(w, "relatedInformation", v.related_information);
}

void lsp::write_fields(json::writer& w, publish_diagnostics_params const& v)
{
    json::put(w, "uri", v.uri);
    json::put(w, "version", v.version);
    json::put(w, "diagnostics", v.diagnostics);
}

void lsp::write_fields(json::writer& w, semantic_tokens_legend const& v)
{
    json::put(w, "tokenTypes", v.token_types);
    json::put(w, "tokenModifiers", v.token_modifiers);
}

void lsp::write_fields(json::writer& w, semantic_tokens const& v)
{
    json::put(w, "data", v.data);
}

void lsp::write_fields(json::writer& w, inlay_hint const& v)
{
    json::put(w, "position", v.position);
    json::put(w, "label", v.label);
    w.write("kind", i32(v.kind));
    if (v.padding_left)
        w.write("paddingLeft", true);
    if (v.padding_right)
        w.write("paddingRight", true);
    if (!v.text_edits.empty())
        json::put(w, "textEdits", v.text_edits);
}

void lsp::write_fields(json::writer& w, code_action const& v)
{
    json::put(w, "title", v.title);
    json::put(w, "kind", cc::string_view("quickfix"));
    if (!v.diagnostics.empty())
        json::put(w, "diagnostics", v.diagnostics);
    if (v.is_preferred)
        w.write("isPreferred", true);
    w.begin_object("edit");
    w.begin_object("changes");
    json::put(w, v.uri, v.edits);
    w.end_object();
    w.end_object();
}

void lsp::write_fields(json::writer& w, log_message_params const& v)
{
    w.write("type", i32(v.type));
    json::put(w, "message", v.message);
}

void lsp::write_fields(json::writer& w, hover const& v)
{
    w.begin_object("contents");
    json::put(w, "kind", cc::string_view("markdown"));
    json::put(w, "value", v.markdown);
    w.end_object();
    json::put(w, "range", v.range);
}

void lsp::write_fields(json::writer& w, completion_item const& v)
{
    json::put(w, "label", v.label);
    w.write("kind", i32(v.kind));
    if (!v.detail.empty())
        json::put(w, "detail", v.detail);
}

void lsp::write_fields(json::writer& w, signature_information const& v)
{
    json::put(w, "label", v.label);
    if (!v.documentation.empty())
    {
        w.begin_object("documentation");
        json::put(w, "kind", cc::string_view("markdown"));
        json::put(w, "value", v.documentation);
        w.end_object();
    }
    w.begin_array("parameters");
    for (auto const& p : v.parameters)
    {
        w.begin_object();
        json::put(w, "label", p);
        w.end_object();
    }
    w.end_array();
}

void lsp::write_fields(json::writer& w, signature_help const& v)
{
    json::put(w, "signatures", v.signatures);
    w.write("activeSignature", v.active_signature);
    w.write("activeParameter", v.active_parameter);
}

void lsp::write_fields(json::writer& w, workspace_edit const& v)
{
    w.begin_object("changes");
    for (auto const& d : v.changes)
        json::put(w, d.uri, d.edits);
    w.end_object();
}

void lsp::write_fields(json::writer& w, prepare_rename_result const& v)
{
    json::put(w, "range", v.range);
    json::put(w, "placeholder", v.placeholder);
}

void lsp::write_fields(json::writer& w, document_symbol const& v)
{
    json::put(w, "name", v.name);
    if (!v.detail.empty())
        json::put(w, "detail", v.detail);
    w.write("kind", i32(v.kind));
    json::put(w, "range", v.range);
    json::put(w, "selectionRange", v.selection_range);
    if (!v.children.empty())
        json::put(w, "children", v.children);
}

void lsp::write_fields(json::writer& w, folding_range const& v)
{
    w.write("startLine", v.start_line);
    w.write("endLine", v.end_line);
    if (v.is_comment)
        json::put(w, "kind", cc::string_view("comment"));
}

void lsp::write_fields(json::writer& w, symbol_information const& v)
{
    json::put(w, "name", v.name);
    w.write("kind", i32(v.kind));
    json::put(w, "location", v.location);
    if (!v.container_name.empty())
        json::put(w, "containerName", v.container_name);
}
