#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/thread/async.hh>
#include <shaped-viewer/asset/asset.hh>
#include <shaped-viewer/asset/asset_loader.hh>
#include <shaped-viewer/asset/impl/asset_import.hh>

namespace sv
{
asset::asset(cc::shared_async<impl::imported_asset> node,
             asset_loader const* loader,
             cc::shared_ptr<cc::atomic<bool>> dropped)
  : _node(cc::move(node)), _dropped(cc::move(dropped)), _loader(loader)
{
}

asset::~asset()
{
    _drop();
}

asset::asset(asset&&) noexcept = default;

asset& asset::operator=(asset&& rhs) noexcept
{
    // The load this one held is dropped here, so it owes the same wait the destructor does.
    if (this != &rhs)
    {
        _drop();
        _node = cc::move(rhs._node);
        _dropped = cc::move(rhs._dropped);
        _loader = rhs._loader;
        _data = cc::move(rhs._data);
        _error = cc::move(rhs._error);
        _ready = rhs._ready;
    }
    return *this;
}

void asset::_drop()
{
    if (_node == nullptr)
        return;

    // Without the wait the worker would go on reading a loader the caller is free to destroy next.
    if (_dropped != nullptr)
        _dropped->store(true);
    (void)cc::try_async_blocking_get(_node);
    _node = nullptr;
}

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

    // Minting runs here, on whichever thread is asking, rather than on the worker.
    // `material_override` is caller code, and running it on the caller's own thread is what lets it touch caller state without a lock.
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
