#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_completion_group.hh>

namespace sg::backend::dx12
{
void dx12_completion_group_pool::initialize(ID3D12Device* device)
{
    CC_ASSERT(device != nullptr, "completion group pool needs a device");
    _device = device;
    _free = std::make_shared<free_list>();
}

dx12_completion_group_handle dx12_completion_group_pool::acquire()
{
    CC_ASSERT(_device != nullptr, "completion group pool used before initialization");

    // The deleter is what makes recycling work, and it holds the free list WEAKLY on purpose: a group outliving its
    // pool is the normal teardown order, and there it must destroy its fence rather than resurrect a dead list.
    auto const recycle = [weak = std::weak_ptr<free_list>(_free)](dx12_completion_group* g)
    {
        auto const list = weak.lock();
        if (list == nullptr)
        {
            delete g;
            return;
        }
        list->groups.lock([&](cc::vector<dx12_completion_group*>& groups) { groups.push_back(g); });
    };

    dx12_completion_group* reused = nullptr;
    _free->groups.lock(
        [&](cc::vector<dx12_completion_group*>& groups)
        {
            if (!groups.empty())
            {
                reused = groups.back();
                groups.pop_back();
            }
        });

    if (reused != nullptr)
        return dx12_completion_group_handle(reused, recycle);

    auto* const fresh = new dx12_completion_group();
    HRESULT const hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fresh->fence));
    if (FAILED(hr))
    {
        delete fresh;
        CC_ASSERT(false, "ID3D12Device::CreateFence (transfer completion group) failed");
        return nullptr;
    }
    return dx12_completion_group_handle(fresh, recycle);
}

void dx12_completion_group_pool::shutdown()
{
    if (_free == nullptr)
        return;

    // Dropping the list is what turns every still-live group's deleter into a plain delete.
    _free->groups.lock(
        [](cc::vector<dx12_completion_group*>& groups)
        {
            for (auto* g : groups)
                delete g;
            groups.clear();
        });
    _free = nullptr;
    _device = nullptr;
}
} // namespace sg::backend::dx12
