#include "cube_app.hh"
#include "cube_document.hh"
#include "picking.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <imgui/imgui.h>

#include <limits> // no cc:: numeric limits yet
#include <nexus/test.hh>

#if SR_HAS_WINDOW

/// A mini editor over a versioned document: pick a cube, change it, watch the history grow, scrub back through it.
///
///     uv run dev.py example vdoc/cube-editor
///
/// What it is here to show, in order of how easy it is to miss:
///
///   - **Every edit is an op.** The history list is the document's actual op DAG, walked first-parent, and the
///     labels come from each op's metadata. Nothing maintains an undo stack.
///   - **Deletion removes nothing.** It writes `$alive = false`; scrub back and the cube returns.
///   - **The camera is not an edit.** It lives in the file's workspace, which creates no op and never dirties.
///   - **Scrubbing is read-only.** History is immutable, so editing is disabled until the slider is back at the end.
///   - **Editing stays fast whatever the history's length**, because a snapshot is advanced onto every accepted op
///     and the typed document is evolved incrementally rather than re-parsed.

namespace
{
using namespace cube_editor;

/// The nearest cube under the cursor, or nothing.
[[nodiscard]] vdoc::entity_id pick(vdoc::document const& doc, orbit_camera const& cam, tg::angle_f fov, tg::pos2f cursor, tg::vec2i viewport)
{
    auto const ray = pick_ray(cam, fov, cursor, viewport);

    auto best = vdoc::entity_id();
    auto best_t = std::numeric_limits<float>::max();
    doc.each<placement>(
        [&](vdoc::entity_id entity, placement const& p)
        {
            auto const hit = ray_aabb_entry(ray, p.box());
            if (hit.has_value() && hit.value() < best_t)
            {
                best_t = hit.value();
                best = entity;
            }
        });
    return best;
}

/// The inspector for one cube. Each committed change is one op, labelled for the history list.
void draw_inspector(document& doc, vdoc::entity_id selected)
{
    auto const* const p = doc.visible().get<placement>(selected);
    auto const* const s = doc.visible().get<style>(selected);
    if (p == nullptr || s == nullptr)
    {
        ImGui::TextUnformatted("nothing selected — click a cube");
        return;
    }

    ImGui::Text("entity: %.*s", int(selected.as_string_view().size()), selected.as_string_view().data());

    // Edited on a copy, then committed only when imgui reports the drag finished.
    // A frame-by-frame commit would work too, but each frame would be its own op — and a fanned drag is the one
    // shape vdoc's incremental path does NOT make cheap, so a real editor chains the frames instead.
    auto edited = *p;
    auto position = tg::vec3f(edited.center - tg::pos3f::zero);
    auto color = s->color;

    ImGui::DragFloat3("position", &position[0], 0.05f);
    ImGui::DragFloat("size", &edited.half_extent, 0.02f, 0.05f, 8.0f);
    ImGui::ColorEdit3("color", &color[0]);

    if (!doc.is_editable())
    {
        ImGui::TextUnformatted("(viewing history — editing is off)");
        return;
    }

    if (ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        edited.center = tg::pos3f::zero + position;
        if (edited.center != p->center || edited.half_extent != p->half_extent)
            doc.set_placement(selected, edited, cc::format("move {}", selected.as_string_view()));
        if (color != s->color)
            doc.set_style(selected, style{.color = color}, cc::format("recolor {}", selected.as_string_view()));
    }

    if (ImGui::Button("delete"))
        doc.remove(selected);
}

/// The history slider, over the document's own first-parent op chain.
void draw_history(document& doc)
{
    auto const count = int(doc.history().size());
    auto revision = doc.revision();

    ImGui::Text("%d revisions", count);
    if (ImGui::SliderInt("revision", &revision, 0, count - 1))
        doc.show_revision(revision);

    ImGui::TextWrapped("%.*s", int(doc.revision_label(revision).size()), doc.revision_label(revision).data());

    if (!doc.is_editable())
    {
        ImGui::TextUnformatted("viewing history — this revision cannot be edited");
        ImGui::SameLine();
        if (ImGui::Button("back to now"))
            doc.show_revision(count - 1);
    }
}
} // namespace

EXAMPLE("vdoc/cube-editor")
{
    auto doc = cube_editor::document::open("cube-editor.vdoc");
    if (!doc.has_value())
    {
        cc::println("no SQLite backend was compiled in — there is nowhere to keep the document");
        return;
    }

    auto app = cube_editor::app::create("vdoc cube editor — click a cube, drag to orbit, close to end");
    if (app == nullptr)
        return; // create() already said what was missing

    auto camera = doc.value().load_camera().value_or(cube_editor::orbit_camera());
    auto selected = vdoc::entity_id();
    auto dragging = false;

    while (app->begin_frame())
    {
        auto& imgui = app->imgui();

        for (auto const& e : app->windows().events())
        {
            if (imgui.wants_mouse())
                continue; // the click belongs to a panel, not to the scene

            if (e.is_mouse_button())
            {
                auto const& b = e.as_mouse_button();
                if (b.button == sr::mouse_button::left && b.is_down)
                    selected = pick(doc.value().visible(), camera, app->vertical_fov(), b.cursor_pos,
                                    app->viewport());
                if (b.button == sr::mouse_button::right)
                    dragging = b.is_down;
            }
            else if (e.is_mouse_move() && dragging)
                camera.orbit(e.as_mouse_move().delta);
            else if (e.is_mouse_wheel())
                camera.zoom(e.as_mouse_wheel().delta[1]);
        }

        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340, 480), ImGuiCond_FirstUseEver);
        ImGui::Begin("cube editor");

        ImGui::TextUnformatted("left-click a cube to select, right-drag to orbit, scroll to zoom");
        ImGui::Separator();

        ImGui::SeparatorText("scene");
        ImGui::Text("%d cubes", int(doc.value().visible().entities().size()));
        if (ImGui::Button("add a cube") && doc.value().is_editable())
            selected = doc.value().add_cube({.center = camera.target, .half_extent = 0.8f}, {});

        ImGui::SeparatorText("selection");
        draw_inspector(doc.value(), selected);

        ImGui::SeparatorText("history");
        draw_history(doc.value());

        ImGui::SeparatorText("file");
        ImGui::Text("saved: %s", doc.value().is_saved() ? "yes" : "no");
        if (ImGui::Button("save"))
            doc.value().save();
        ImGui::TextWrapped("Publishing derives the ops from the refs by reachability, so an op no ref reaches "
                           "cannot be written by mistake.");

        ImGui::End();

        app->end_frame(doc.value().visible(), camera, selected);
    }

    doc.value().store_camera(camera);
    doc.value().save();
    cc::println("{} revisions in the document; reopen to find it exactly as you left it",
                doc.value().history().size());
}

#endif // SR_HAS_WINDOW
