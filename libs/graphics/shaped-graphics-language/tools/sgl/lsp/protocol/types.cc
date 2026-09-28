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

void lsp::write_fields(json::writer& w, log_message_params const& v)
{
    w.write("type", i32(v.type));
    json::put(w, "message", v.message);
}
