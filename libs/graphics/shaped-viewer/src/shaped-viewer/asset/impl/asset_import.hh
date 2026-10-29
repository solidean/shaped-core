#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/asset/asset_data.hh>
#include <shaped-viewer/asset/uri_resolver.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/material/material_attribute.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>

/// The per-format importers behind `sv::asset_loader`.
///
/// One free function per format, taking an already-parsed babel document — which is what makes the loader's
/// already-parsed overloads the same code path as its byte and uri ones, rather than a second entry point.
/// Internal: a caller reaches these through `asset_loader::load`.

namespace sv::impl
{
/// One material an import built, before any library has seen it.
///
/// The split exists because minting runs the loader's `material_override`, which is caller code.
/// The expensive half of an import — decoding images, reading vertex buffers, building tangent frames — wants to run off the calling thread, and the hook does not.
/// So an importer produces these, and `acquire_asset_materials` is the small step in `sv::asset::poll` that turns them into ids and points the meshes at them.
/// Running there, on the calling thread, is what lets the hook touch caller state without a lock.
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
/// Runs on the thread calling `sv::asset::poll`, because the loader's `material_override` hook runs here and is caller code.
/// `out.materials` is parallel to `definitions`, and each slot's own `meshes` is what gets pointed at the new id — so
/// two of a file's materials that hash alike still move independently.
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

/// FBX into `asset_data`, one mesh per (node, material slot) — FBX connects materials to the node placing a mesh, so two instances of one geometry may draw with different materials.
///
/// `doc` should have been read with `read_options::triangulate`; a polygon that was not is fanned, with an issue.
/// Every position comes out in sv's convention: meters, +Y up, right-handed, whatever axes and unit the file declared.
/// `resolve` is what a texture's file name is fetched through, once the importer has checked it and joined it onto `base_uri`'s directory;
/// a texture it cannot fetch or decode is an issue, and its material keeps its factors.
[[nodiscard]] cc::result<asset_data> import_fbx(babel::fbx::data const& doc,
                                                asset_loader_config const& cfg,
                                                cc::string_view asset_name,
                                                uri_resolver resolve,
                                                cc::string_view base_uri,
                                                cc::vector<asset_material_definition>& definitions);

/// One decoded image expanded to tightly packed rgba8.
///
/// Every decode lands on rgba8 whatever the file held, because that is the one shape `texture_data` and the bindless table agree on today.
/// A 16-bit PNG keeps its high byte and an f32 image is clamped, both of which lose precision the importer has nowhere to put — block compression and wider formats are a `derived` recipe, not this.
[[nodiscard]] cc::result<cc::vector<byte>> to_rgba8(babel::image::image const& img);

/// Makes every light id unique within the asset: a name that is empty or shared becomes `name##i`, or `light##i` for an empty one.
/// `i` is its index in `lights`, which keeps what a human reads while giving each its own identity.
///
/// Which names are shared is settled before any is renamed, since renaming the first of two would otherwise leave the second looking unique.
/// A generated id can still meet a name the file already uses — `a##1` beside two `a`s — so a taken one bumps its suffix until it is new.
void disambiguate_light_ids(cc::vector<asset_light>& lights);

/// Tangent frames aligned with a uv set, for a mesh that carries normals and uvs but no tangents.
///
/// Per triangle, the tangent is the direction of increasing u and the bitangent that of increasing v.
/// Both are accumulated per vertex, weighted by the triangle's size, then made perpendicular to the vertex normal.
/// `handedness` is -1 where the bitangent opposes `cross(normal, tangent)`, which is a mirrored uv island.
/// `uvs` must be in the convention whose +v is "up" in a normal map's tangent space — OpenGL's, V up the image.
/// So a caller holding top-row-first uvs passes them unflipped from the file, before its own flip.
/// A vertex no triangle gives a usable tangent falls back to an arbitrary one perpendicular to its normal.
struct uv_tangent_frames
{
    cc::vector<tg::quat_f> frames;
    cc::vector<f32> handedness;
};
[[nodiscard]] uv_tangent_frames tangent_frames_from_uvs(cc::span<tg::pos3f const> positions,
                                                        cc::span<tg::vec3f const> normals,
                                                        cc::span<tg::vec2f const> uvs,
                                                        cc::span<u32 const> indices);

/// Whether `n` can be normalized into a frame at all.
///
/// A zero, infinite or NaN normal is what a degenerate triangle and a careless exporter both produce, and normalizing
/// one yields a NaN quaternion the hit shader then trusts — which is strictly worse than supplying no frame, since the
/// geometric fallback is correct and a NaN is not shading at all.
/// So an importer tests this BEFORE building a frame, and drops the whole attribute for a mesh that fails it.
/// Written as a self-comparison plus a bound, which predates `cc::is_finite` in `clean-core/math/float.hh`.
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
