#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/thread/async.hh>
#include <shaped-viewer/asset/asset.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>

namespace sv
{
asset::asset(cc::shared_async<impl::imported_asset> node, asset_loader const* loader)
  : _node(cc::move(node)), _loader(loader)
{
}

asset::~asset() = default;
asset::asset(asset&&) noexcept = default;
asset& asset::operator=(asset&&) noexcept = default;

bool asset::is_valid() const
{
    return _node != nullptr || _ready || !_error.empty();
}

bool asset::poll()
{
    if (_ready || !_error.empty() || _node == nullptr)
        return _ready;

    if (!_node->is_ready())
        return false; // still running, and asking must not block

    if (_node->has_error())
    {
        _error = _node->try_error()->underlying().to_string();
        _node = nullptr;
        return false;
    }

    auto const* const imported = _node->try_value();
    CC_ASSERT(imported != nullptr, "a settled node without an error carries a value");

    _data = imported->data;

    // The one step that could not run on the worker: `material_library` is not thread-safe, so minting what the
    // importer described happens here, on whichever thread is asking.
    CC_ASSERT(_loader != nullptr, "an asset in flight names the loader whose config finishes it");
    if (auto const lib = _loader->_library(); lib.has_value())
        impl::acquire_asset_materials(_data, imported->definitions, _loader->config(), *lib.value());
    else
        _data.issues.push_back("shaped-viewer: no material library to mint this asset's materials into");

    // Released as soon as it is collected: the payload is in `_data` now, and holding the node would keep a second
    // copy of every buffer alive for as long as the asset is.
    _node = nullptr;
    _ready = true;
    return true;
}

bool asset::wait()
{
    if (_node != nullptr)
        (void)cc::try_async_blocking_get(_node);
    return poll();
}
} // namespace sv
