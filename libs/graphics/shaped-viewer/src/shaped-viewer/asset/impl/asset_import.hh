#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/asset/asset_data.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/material_attribute.hh>

/// The per-format importers behind `sv::asset_loader`.
///
/// One free function per format, taking an already-parsed babel document — which is what makes the loader's
/// already-parsed overloads the same code path as its byte and uri ones, rather than a second entry point.
/// Internal: a caller reaches these through `asset_loader::load`.

namespace sv::impl
{
/// One material an import built, before any library has seen it.
///
/// The split exists for one reason: `material_library` is not thread-safe, and the expensive half of an import —
/// decoding images, reading vertex buffers, building tangent frames — is exactly what wants to run off the main
/// thread.
/// So an importer produces these, and `acquire_asset_materials` is the small main-thread step that turns them into
/// ids and points the meshes at them.
struct asset_material_definition
{
    cc::string name; ///< the file's own, matching `asset_material::name` at the same index
    cc::vector<material_attribute_binding> bindings;
};

/// Everything an import produced without touching a library: the asset, and the materials still to be minted.
/// The value a `load_async` node carries, and what `sv::asset::poll` finishes.
struct imported_asset
{
    asset_data data;
    cc::vector<asset_material_definition> definitions;
};

/// The definitions an import produced, minted into `lib` and written back onto `out`.
///
/// Must run where the library is owned; everything before it is thread-safe.
/// `out.materials` is parallel to `definitions`, and each slot's own `meshes` is what gets pointed at the new id — so
/// two of a file's materials that hash alike still move independently.
/// The loader's `material_override` hook runs here too, since what it returns is a library id.
void acquire_asset_materials(asset_data& out,
                             cc::span<asset_material_definition const> definitions,
                             asset_loader_config const& cfg,
                             material_library& lib);

/// glTF 2.0 into `asset_data`, one mesh per (primitive, material).
///
/// Touches no library: the materials it built come back in `definitions`, for `acquire_asset_materials` to mint.
/// That is what lets the whole of this run on a worker.
/// Never fails on a file it can partially read: an unusable primitive, an undecodable image or an unsupported feature
/// becomes an issue on the result, and only a document with nothing at all to import is an error.
///
/// EVERY mesh crosses, and `scene` / `default_scene` are not read.
/// Which arrangement a file called default is a decision about what the caller wanted rather than about what the file
/// contains, and honouring it would drop meshes `asset_data::find_mesh` is then asked for and cannot answer.
[[nodiscard]] cc::result<asset_data> import_gltf(babel::gltf::data const& doc,
                                                 asset_loader_config const& cfg,
                                                 cc::string_view asset_name,
                                                 cc::vector<asset_material_definition>& definitions);

/// Wavefront OBJ into `asset_data`, one mesh per `usemtl` run.
///
/// The importer owes triangulation and vertex dedup, and does both here.
/// `.mtl` is planned in babel and deferred here, so an OBJ import carries geometry plus material NAMES only — each name
/// becomes an unbound `openpbr` definition so the slot exists to be overridden.
[[nodiscard]] cc::result<asset_data> import_obj(babel::obj::data const& doc,
                                                asset_loader_config const& cfg,
                                                cc::string_view asset_name,
                                                cc::vector<asset_material_definition>& definitions);

/// STL into `asset_data`: one file is one mesh, since STL carries no materials and no hierarchy.
///
/// The positions go across as the raw triangle list they already are, and the per-facet normals are dropped — the hit
/// shader computes the geometric frame from the triangle anyway, so storing one could only ever match it.
/// STL carries no materials, so it produces no definitions — the parameter is there for one uniform call shape.
[[nodiscard]] cc::result<asset_data> import_stl(babel::stl::data const& doc,
                                                asset_loader_config const& cfg,
                                                cc::string_view asset_name,
                                                cc::vector<asset_material_definition>& definitions);

/// Whether `n` can be normalized into a frame at all.
///
/// A zero, infinite or NaN normal is what a degenerate triangle and a careless exporter both produce, and normalizing
/// one yields a NaN quaternion the hit shader then trusts — which is strictly worse than supplying no frame, since the
/// geometric fallback is correct and a NaN is not shading at all.
/// So an importer tests this BEFORE building a frame, and drops the whole attribute for a mesh that fails it.
/// Written as a self-comparison plus a bound because `cc` has no float classification yet
/// (libs/base/clean-core/docs/TODO.md).
[[nodiscard]] bool is_usable_normal(tg::vec3f n);

/// The tangent frame a supplied normal and glTF TANGENT describe, as a rotation taking tangent space to object space.
///
/// The tangent is re-orthogonalized against the normal, because an interpolated or authored TANGENT is rarely exactly
/// perpendicular and a basis that is not orthonormal is not a rotation.
/// `handedness` is glTF's `TANGENT.w` and travels beside the frame, since no rotation carries a mirror.
[[nodiscard]] tg::quat_f tangent_frame_of(tg::vec3f normal, tg::vec3f tangent);

/// The frame a normal ALONE describes, with an arbitrary tangent perpendicular to it.
///
/// Correct for everything that does not read the tangent direction — which is anisotropy and normal mapping, and those
/// want generated, uv-aligned tangents rather than this.
/// Still much better than dropping the normal: the geometric fallback would give the FACE normal, and a smooth surface
/// would come back faceted.
[[nodiscard]] tg::quat_f tangent_frame_of(tg::vec3f normal);
} // namespace sv::impl
