#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh> // the node is held by value, so cc::shared_async must be complete
#include <shaped-viewer/asset/asset_data.hh>
#include <shaped-viewer/fwd.hh>

/// A load in flight, and the asset it produces.
///
/// Handed back by `asset_loader::load_async`.
///
///     auto car = loader.load_async("car.glb");
///     for (auto frame : viewer.frames())
///     {
///         car.poll();
///         auto scene = frame.add_scene();
///         for (auto const& m : car.meshes())   // empty until the load lands, then all of them
///             scene.add_mesh(m);
///     }
///
/// **What runs where.** Fetching, parsing and importing are work over bytes, and run on whatever scheduler `cc::async`
/// was given — a `cc::scoped_default_async_scheduler`, or none, in which case nothing progresses until something
/// drives it and `wait` is that something.
/// The one step that cannot move off the calling thread is minting the imported materials, since `material_library` is
/// not thread-safe: `poll` is where that happens, which is why it is a call rather than a query.
///
/// **`is_ready` is whole-asset, not structure-first.**
/// The design in libs/graphics/shaped-viewer/docs/asset-loading.md wants the mesh list and its bounds to arrive ahead
/// of the payloads, so a placeholder can stand in per mesh.
/// That needs a mesh whose geometry has not been read at all, which `create_mesh` has no form for yet — so for now an
/// asset is either not there or entirely there, and what arrives progressively is the upload, which the resource
/// managers already stream.
///
/// Move-only: it owns the load, and dropping it drops the load.
class sv::asset
{
public:
    asset() = default;
    ~asset();

    asset(asset&&) noexcept;
    asset& operator=(asset&&) noexcept;
    asset(asset const&) = delete;
    asset& operator=(asset const&) = delete;

    /// Whether this names a load at all; a default-constructed one does not.
    [[nodiscard]] bool is_valid() const;

    /// Collects the load if it has finished, minting its materials — and returns whether the asset is now available.
    ///
    /// Cheap and idempotent: once collected it does nothing, so calling it every frame is the intended shape.
    /// It never blocks, so a load still running simply answers false.
    bool poll();

    /// Whether the load finished and produced an asset.
    /// Only ever true after a `poll` that saw it land.
    [[nodiscard]] bool is_ready() const { return _ready; }

    /// Whether the load finished and did not produce one; `error` says why.
    [[nodiscard]] bool has_error() const { return !_error.empty(); }
    [[nodiscard]] cc::string_view error() const { return _error; }

    /// Drives the load to completion on THIS thread, then collects it.
    ///
    /// With a scheduler installed the work is likely already done and this only collects; with none, this is what runs
    /// the whole thing.
    /// Either way it is what a caller who cannot proceed without the asset uses.
    /// Returns whether an asset came out of it.
    bool wait();

    /// The loaded asset, or an empty one while the load is still running or if it failed.
    ///
    /// Deliberately not an assert: the caller's shape is a frame loop asking every frame, and a query answering
    /// "nothing yet" needs no branch around it.
    [[nodiscard]] asset_data const& data() const { return _data; }

    /// The meshes the load produced, empty until it has.
    [[nodiscard]] cc::span<sv::mesh_data const> meshes() const { return _data.meshes; }

    /// Everything the importer had to say, empty until the load lands.
    [[nodiscard]] cc::span<cc::string const> issues() const { return _data.issues; }

    // implementation
public:
    /// Wraps a load already in flight — `asset_loader::load_async` is what builds one.
    /// `loader` must outlive the load: its config is what the collecting step reads.
    asset(cc::shared_async<impl::imported_asset> node, asset_loader const* loader);

private:
    /// The worker half's result, released once collected.
    cc::shared_async<impl::imported_asset> _node;

    asset_loader const* _loader = nullptr;

    asset_data _data;
    cc::string _error;
    bool _ready = false;
};
