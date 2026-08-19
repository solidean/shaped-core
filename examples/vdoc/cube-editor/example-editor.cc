#include "cube_app.hh"
#include "cube_document.hh"
#include "picking.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <imgui/imgui.h>
#include <nexus/test.hh>

#include <limits> // no cc:: numeric limits yet

#if SR_HAS_WINDOW

/// A mini editor over a versioned document: pick a cube, drag it, watch the history grow, scrub back through it.
///
///     uv run dev.py example vdoc/cube-editor
///
/// What it is here to show, in order of how easy it is to miss:
///
///   - **Nothing is ever edited in place.** A document IS an op, and every change builds a new one on top of it.
///     The timeline is that chain of ops walked first-parent, labelled from each op's own metadata — there is no undo
///     stack anywhere in this example.
///   - **Editing a past revision is ordinary.** Scrub back, move a cube, and you have branched.
///     A DAG has no opinion about that, so nothing here needs a mode or a guard.
///   - **A drag is hundreds of ops and one history entry.** Every frame chains an op onto the last, so the document
///     evolves incrementally instead of re-parsing; on release those frames are dropped and one op from start to
///     finish takes their place.
///   - **Deletion removes nothing.** It writes `$alive = false`; scrub back and the cube returns.
///   - **The camera is not an edit.** It lives in the file's workspace, which creates no op and never dirties.

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

/// Wraps the imgui widget on the line above into a continuous edit.
///
/// `write` runs on every frame the widget reports a change, so the view is live; the whole gesture becomes one op
/// when the widget is released, whatever it did in between.
template <class F>
void as_continuous_edit(document& doc, bool changed, F&& write, cc::string_view label)
{
    if (ImGui::IsItemActivated())
        doc.begin_continuous_edit();
    if (changed)
        write();
    // IsItemDeactivated rather than its AfterEdit sibling: a drag released back where it started still has to end,
    // and the collapse writes nothing when the net change is nothing.
    if (ImGui::IsItemDeactivated())
        doc.end_continuous_edit(label);
}

/// The inspector for one cube.
void draw_inspector(document& doc, vdoc::entity_id selected)
{
    auto const* const p = doc.current().get<placement>(selected);
    auto const* const s = doc.current().get<style>(selected);
    if (p == nullptr || s == nullptr)
    {
        ImGui::TextUnformatted("nothing selected — click a cube");
        return;
    }

    ImGui::Text("entity: %.*s", int(selected.as_string_view().size()), selected.as_string_view().data());

    auto edited = *p;
    auto position = tg::vec3f(edited.center - tg::pos3f::zero);
    auto color = s->color;

    auto const moved = ImGui::DragFloat3("position", &position[0], 0.05f);
    as_continuous_edit(
        doc, moved,
        [&]
        {
            edited.center = tg::pos3f::zero + position;
            doc.set_placement(selected, edited, "move");
        },
        cc::format("move {}", selected.as_string_view()));

    auto const resized = ImGui::DragFloat("size", &edited.half_extent, 0.02f, 0.05f, 8.0f);
    as_continuous_edit(
        doc, resized, [&] { doc.set_placement(selected, edited, "resize"); },
        cc::format("resize {}", selected.as_string_view()));

    auto const recolored = ImGui::ColorEdit3("color", &color[0]);
    as_continuous_edit(
        doc, recolored, [&] { doc.set_style(selected, style{.color = color}, "recolor"); },
        cc::format("recolor {}", selected.as_string_view()));

    if (ImGui::Button("delete"))
        doc.remove(selected);
}

/// The timeline, over the document's own first-parent op chain.
/// Moving it writes no op at all — it only changes which op the document is derived from.
void draw_timeline(document& doc)
{
    auto const count = int(doc.timeline().size());
    auto revision = doc.revision();

    ImGui::Text("%d revisions", count);
    if (ImGui::SliderInt("revision", &revision, 0, count - 1))
        doc.show_revision(revision);

    ImGui::TextWrapped("%.*s", int(doc.revision_label(revision).size()), doc.revision_label(revision).data());

    if (revision != count - 1)
    {
        ImGui::TextWrapped("Editing from here branches — the revisions after this one are simply left behind.");
        if (ImGui::Button("back to the newest"))
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

    auto app = cube_editor::app::create("vdoc cube editor — click a cube, right-drag to orbit, close to end");
    if (app == nullptr)
        return; // create() already said what was missing

    auto camera = doc.value().load_camera().value_or(cube_editor::orbit_camera());
    auto selected = vdoc::entity_id();
    auto orbiting = false;

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
                    selected = pick(doc.value().current(), camera, app->vertical_fov(), b.cursor_pos, app->viewport());
                if (b.button == sr::mouse_button::right)
                    orbiting = b.is_down;
            }
            else if (e.is_mouse_move() && orbiting)
                camera.orbit(e.as_mouse_move().delta);
            else if (e.is_mouse_wheel())
                camera.zoom(e.as_mouse_wheel().delta[1]);
        }

        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 520), ImGuiCond_FirstUseEver);
        ImGui::Begin("cube editor");

        ImGui::TextUnformatted("left-click a cube to select, right-drag to orbit, scroll to zoom");
        ImGui::Separator();

        ImGui::SeparatorText("scene");
        ImGui::Text("%d cubes", int(doc.value().current().entities().size()));
        if (ImGui::Button("add a cube"))
            selected = doc.value().add_cube({.center = camera.target, .half_extent = 0.8f}, {});

        ImGui::SeparatorText("selection");
        draw_inspector(doc.value(), selected);

        ImGui::SeparatorText("timeline");
        draw_timeline(doc.value());

        ImGui::SeparatorText("file");
        ImGui::Text("saved: %s", doc.value().is_saved() ? "yes" : "no");
        if (ImGui::Button("save"))
            doc.value().save();
        ImGui::TextWrapped("Publishing derives the ops from the refs by reachability, so an op no ref reaches "
                           "cannot be written by mistake.");

        ImGui::End();

        app->end_frame(doc.value().current(), camera, selected);
    }

    doc.value().store_camera(camera);
    doc.value().save();
    cc::println("{} revisions in the document; reopen to find it exactly as you left it", doc.value().timeline().size());
}

#endif // SR_HAS_WINDOW
