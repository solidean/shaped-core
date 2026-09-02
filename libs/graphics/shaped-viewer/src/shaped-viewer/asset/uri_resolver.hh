#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-viewer/fwd.hh>

/// Turning a uri into bytes, which is the only thing in the importer that touches the outside world.
///
/// **Nothing in the importer opens a file.**
/// The core entry points take bytes plus a resolver, which is the shape `babel::gltf::read_options::resolve_uri` already
/// has — so a glTF's external buffers and images travel the same seam its own bytes did.
///
/// This is explicitly an INTERMEDIATE.
/// A real virtual filesystem in clean-core supersedes it, and the signature is chosen so that migration touches one
/// function.
/// Handing back a `cc::pinned_data` is what keeps glTF's zero-copy property intact through the importer.

namespace sv
{
/// The per-call form: a borrowed resolver, passed down into a load.
/// Non-owning, so whatever it refers to must outlive the call.
using uri_resolver = cc::function_ref<cc::result<cc::pinned_data<byte const>>(cc::string_view uri)>;

/// The owning form the process-wide hook and `asset_loader_config` hold.
/// Owning rather than borrowed because both outlive the expression that set them.
using uri_resolver_provider = cc::unique_function<cc::result<cc::pinned_data<byte const>>(cc::string_view uri)>;

/// Sets the hook every uri-based load resolves through, process-wide.
///
///     sv::set_resolve_uri([&](cc::string_view uri) { return my_vfs.read(uri); });
///
/// Unset by default, and then `impl::resolve_uri_from_filesystem` answers instead — so a caller who never touches this
/// loads from disk, and a host with a virtual filesystem replaces it once rather than threading a resolver through
/// every call.
/// Passing `{}` restores the filesystem default.
void set_resolve_uri(uri_resolver_provider provider);

/// The bytes behind `uri`: the caller's hook if they set one, otherwise the filesystem default.
[[nodiscard]] cc::result<cc::pinned_data<byte const>> resolve_uri(cc::string_view uri);

namespace impl
{
/// Reads `uri` as a path through `cc::file_read_stream_adapter`, percent-decoding it first.
/// The default when no hook is set, and what makes "load a file" work with no ceremony.
[[nodiscard]] cc::result<cc::pinned_data<byte const>> resolve_uri_from_filesystem(cc::string_view uri);

/// `%20` and friends decoded; every other byte copied through.
/// A lone `%` or a malformed escape is kept verbatim rather than being an error — a uri is a caller's string, and a
/// path that happens to contain a percent must still open.
[[nodiscard]] cc::string percent_decode(cc::string_view uri);

/// `relative` resolved against the directory `base` sits in.
///
/// An absolute `relative` — one carrying a scheme (`http:`, `data:`), starting with `/` or `\`, or opening with a
/// Windows drive letter — comes back untouched, since it names its own location.
/// An empty `base` likewise leaves `relative` alone.
/// This lives here rather than in babel because babel deliberately owns no filesystem policy.
[[nodiscard]] cc::string join_uri(cc::string_view base, cc::string_view relative);

/// The directory part of `uri`, empty when it names no directory.
/// Includes the trailing separator.
[[nodiscard]] cc::string_view directory_of(cc::string_view uri);

/// The lowercased extension of `uri`, without the dot, empty when it has none.
/// Any query or fragment is dropped first, so `model.glb#mesh3` still reports `glb`.
[[nodiscard]] cc::string extension_of(cc::string_view uri);
} // namespace impl
} // namespace sv
