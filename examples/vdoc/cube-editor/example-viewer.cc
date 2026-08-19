#include "cube_app.hh"
#include "cube_document.hh"

#include <clean-core/string/print.hh>
#include <imgui/imgui.h>
#include <nexus/test.hh>

#if SR_HAS_WINDOW

namespace
{
/// What this example's imgui layout is filed under in the workspace, distinct from the editor's.
constexpr cc::string_view ui_name = "viewer";
} // namespace

/// The whole stack on one screen, with nothing on top of it.
///
/// A `.vdoc` file holds the scene, its cubes are drawn through sg, and the camera lives in the file's WORKSPACE —
/// so orbiting and quitting and coming back puts you exactly where you left off, without ever having edited the
/// document. That last part is the one worth watching for: `is_saved` never goes false when you move the camera.
///
///     uv run dev.py example vdoc/cube-viewer
///
EXAMPLE("vdoc/cube-viewer")
{
    auto doc = cube_editor::document::open("cube-editor.vdoc");
    if (!doc.has_value())
    {
        cc::println("no SQLite backend was compiled in — there is nowhere to keep the document");
        return;
    }

    auto app = cube_editor::app::create("vdoc cube viewer — drag to orbit, scroll to zoom, close to end");
    if (app == nullptr)
        return; // create() already said what was missing

    // The panel layout the last session left behind, restored before the first frame.
    // Under this example's own name: the editor opens the same file, and its layout is not this one's.
    if (auto const ini = doc.value().load_ui_settings(ui_name); ini.has_value())
        app->imgui().load_settings(ini.value());

    // The camera the last session left behind, or a default for a first run.
    auto camera = doc.value().load_camera().value_or(cube_editor::orbit_camera());

    cc::println("{} entities over {} revisions; drag to orbit, scroll to zoom",
                doc.value().current().entities().size(), doc.value().timeline().size());

    auto dragging = false;
    while (app->begin_frame())
    {
        for (auto const& e : app->windows().events())
        {
            if (e.is_mouse_button())
            {
                auto const& b = e.as_mouse_button();
                if (b.button == sr::mouse_button::left)
                    dragging = b.is_down && !app->imgui().wants_mouse();
            }
            else if (e.is_mouse_move() && dragging)
                camera.orbit(e.as_mouse_move().delta);
            else if (e.is_mouse_wheel() && !app->imgui().wants_mouse())
                camera.zoom(e.as_mouse_wheel().delta[1]);
        }

        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::Begin("cube viewer");
        ImGui::Text("%d cubes", int(doc.value().current().entities().size()));
        ImGui::Text("%d revisions", int(doc.value().timeline().size()));
        ImGui::Separator();
        ImGui::TextWrapped("The camera is workspace state, not document state: moving it writes no op and leaves "
                           "the document saved.");
        ImGui::Text("saved: %s", doc.value().is_saved() ? "yes" : "no");
        ImGui::TextWrapped("Run vdoc/cube-editor to change the scene.");
        ImGui::End();

        app->end_frame(doc.value().current(), camera, vdoc::entity_id());

        // Empty on nearly every frame: imgui only asks for a save a few seconds after something moved.
        if (auto const ini = app->imgui().take_dirty_settings(); ini.has_value())
            doc.value().store_ui_settings(ui_name, ini.value());
    }

    // Written on every exit, and flushed by the store's close(). No op, no ref move, no dirty document.
    doc.value().store_ui_settings(ui_name, app->imgui().settings());
    doc.value().store_camera(camera);
    cc::println("camera stored in the workspace — reopen to land exactly here");
}

#endif // SR_HAS_WINDOW
