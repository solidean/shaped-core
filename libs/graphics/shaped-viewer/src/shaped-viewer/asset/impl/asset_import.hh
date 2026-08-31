#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/asset/asset_data.hh>
#include <shaped-viewer/fwd.hh>

/// The per-format importers behind `sv::asset_loader`.
///
/// One free function per format, taking an already-parsed babel document — which is what makes the loader's
/// already-parsed overloads the same code path as its byte and uri ones, rather than a second entry point.
/// Internal: a caller reaches these through `asset_loader::load`.

namespace sv::impl
{
/// glTF 2.0 into `asset_data`, one mesh per (primitive, material).
///
/// `lib` must carry the `openpbr` material type, since that is what an imported material is an instance of.
/// Never fails on a file it can partially read: an unusable primitive, an undecodable image or an unsupported feature
/// becomes an issue on the result, and only a document with nothing at all to import is an error.
[[nodiscard]] cc::result<asset_data> import_gltf(babel::gltf::data const& doc,
                                                 asset_loader_config const& cfg,
                                                 material_library& lib,
                                                 cc::string_view asset_name);

/// Wavefront OBJ into `asset_data`, one mesh per `usemtl` run.
///
/// The importer owes triangulation and vertex dedup, and does both here.
/// `.mtl` is planned in babel and deferred here, so an OBJ import carries geometry plus material NAMES only — each name
/// is minted as an unbound `openpbr` material so the slot exists to be overridden.
[[nodiscard]] cc::result<asset_data> import_obj(babel::obj::data const& doc,
                                                asset_loader_config const& cfg,
                                                material_library& lib,
                                                cc::string_view asset_name);

/// STL into `asset_data`: one file is one mesh, since STL carries no materials and no hierarchy.
///
/// The positions go across as the raw triangle list they already are, and the per-facet normals are dropped — the hit
/// shader computes the geometric frame from the triangle anyway, so storing one could only ever match it.
[[nodiscard]] cc::result<asset_data> import_stl(babel::stl::data const& doc,
                                                asset_loader_config const& cfg,
                                                material_library& lib,
                                                cc::string_view asset_name);

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
