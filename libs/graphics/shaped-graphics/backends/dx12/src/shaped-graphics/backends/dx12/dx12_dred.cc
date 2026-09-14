// DRED: arming it before device creation, and rendering what it holds after a removal.
// Device loss, and what this adds to its reason, are in libs/graphics/shaped-graphics/docs/concepts/context.md.

#include <clean-core/string/conversion.hh> // cc::utf16_to_utf8
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/dx12/dx12_dred.hh>

namespace sg::backend::dx12
{
namespace
{
/// How many breadcrumbs to print per command list, counting back from the last one it completed.
///
/// The tail is the whole value: a hung list's earlier operations all completed, and the first one that did not
/// is what the removal is about.
constexpr u32 k_breadcrumb_tail = 16;

[[nodiscard]] cc::string_view breadcrumb_op_name(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:
        return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:
        return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:
        return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:
        return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:
        return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:
        return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:
        return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:
        return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:
        return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:
        return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:
        return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:
        return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:
        return "ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:
        return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:
        return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE:
        return "ExecuteBundle";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT:
        return "Present";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE:
        return "BuildRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS:
        return "DispatchRays";
    default:
        return "op";
    }
}

[[nodiscard]] cc::string_view allocation_type_name(D3D12_DRED_ALLOCATION_TYPE type)
{
    switch (type)
    {
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE:
        return "command queue";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR:
        return "command allocator";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:
        return "pipeline state";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:
        return "command list";
    case D3D12_DRED_ALLOCATION_TYPE_FENCE:
        return "fence";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP:
        return "descriptor heap";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP:
        return "heap";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP:
        return "query heap";
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:
        return "resource";
    case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT:
        return "state object";
    default:
        return "allocation";
    }
}

/// A DRED object name, which is wide and may be absent.
void append_name(cc::string& out, wchar_t const* name)
{
    if (name == nullptr)
        return;

    static_assert(sizeof(wchar_t) == sizeof(char16_t), "dx12 is Windows-only, where a wide character is UTF-16");

    isize length = 0;
    while (name[length] != 0)
        ++length;

    out += " \"";
    out += cc::utf16_to_utf8(cc::span<char16_t const>(reinterpret_cast<char16_t const*>(name), length));
    out += '"';
}

/// Templated over the node version for the same reason the page-fault helper is: the two differ in fields
/// this does not read.
template <class NodeT>
void append_breadcrumbs(cc::string& out, NodeT const* node)
{
    for (; node != nullptr; node = node->pNext)
    {
        // A list whose every operation completed did not hang, and printing it would bury the one that did.
        auto const done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0u;
        if (done >= node->BreadcrumbCount)
            continue;

        out += "\n  command list";
        append_name(out, node->pCommandListDebugNameW);
        out += " on queue";
        append_name(out, node->pCommandQueueDebugNameW);
        out += cc::format(": {} of {} operations completed", done, node->BreadcrumbCount);

        auto const first = done > k_breadcrumb_tail ? done - k_breadcrumb_tail : 0u;
        for (auto i = first; i < node->BreadcrumbCount && i < done + 1; ++i)
            out += cc::format("\n    [{}] {}{}", i, breadcrumb_op_name(node->pCommandHistory[i]),
                              i == done ? "   <-- did not complete" : "");
    }
}

/// Templated over the output version because the two differ only in their node type, and the fields read here
/// are in both.
template <class PageFaultOutputT>
void append_page_fault(cc::string& out, PageFaultOutputT const& fault)
{
    if (fault.PageFaultVA == 0)
        return;

    out += cc::format("\n  page fault at GPU VA 0x{:016X}", u64(fault.PageFaultVA));

    // The two lists are the allocations that contained the address: one still live, one already freed.
    // A hit in the freed list is the more useful answer — it names a use-after-free rather than an overrun.
    for (auto const* n = fault.pHeadExistingAllocationNode; n != nullptr; n = n->pNext)
    {
        out += cc::format("\n    live {}", allocation_type_name(n->AllocationType));
        append_name(out, n->ObjectNameW);
    }
    for (auto const* n = fault.pHeadRecentFreedAllocationNode; n != nullptr; n = n->pNext)
    {
        out += cc::format("\n    freed {}", allocation_type_name(n->AllocationType));
        append_name(out, n->ObjectNameW);
    }
}
} // namespace

bool enable_dred_once()
{
    // Process-wide and once-only, for the same reason EnableDebugLayer is: the settings object configures the
    // runtime rather than a device, and two contexts coming up at once must not race on it.
    static bool const enabled = []
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
            return false;

        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        return true;
    }();
    return enabled;
}

cc::string dred_report(ID3D12Device* device)
{
    if (device == nullptr)
        return {};

    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return {};

    cc::string out;

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        append_breadcrumbs(out, breadcrumbs.pHeadAutoBreadcrumbNode);

    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)))
        append_page_fault(out, fault);

    if (out.empty())
        return {};
    return cc::format("\nDRED:{}", out);
}
} // namespace sg::backend::dx12
