#include <clean-core/string/format.hh>
#include <shaped-graphics-language/builtins/register.hh>

using namespace sgl;
using namespace sgl::builtins;

// The barriers of a compute workgroup: each an execution barrier and a memory barrier of one kind of memory.
// A memory-only barrier is HLSL's and no other target's, so SGL has none.

namespace
{
/// A test runs one invocation, which has nobody to wait for.
void nothing(cc::span<check::scalar const>, cc::vector<check::scalar>&)
{
}

struct barrier
{
    cc::string_view name;
    cc::string_view hlsl;
    cc::string_view wgsl;
    /// The `mem_flags` of Metal's one barrier.
    cc::string_view msl_memory;
    cc::string_view doc;
};

constexpr barrier k_barriers[] = {
    {"workgroup_barrier", "GroupMemoryBarrierWithGroupSync", "workgroupBarrier", "mem_threadgroup",
     "/// Waits for every thread of the workgroup, after which each sees what the others wrote to workgroup memory."},
    {"storage_barrier", "DeviceMemoryBarrierWithGroupSync", "storageBarrier", "mem_device",
     "/// Waits for every thread of the workgroup, after which each sees what the others wrote to buffers."},
    {"texture_barrier", "DeviceMemoryBarrierWithGroupSync", "textureBarrier", "mem_texture",
     "/// Waits for every thread of the workgroup, after which each sees what the others stored to images."},
};

written write_barrier(call_context const& ctx)
{
    auto const& b = k_barriers[ctx.data];
    switch (ctx.target)
    {
    case language::hlsl:
        return {.text = cc::format("{}()", b.hlsl)};
    case language::wgsl:
        return {.text = cc::format("{}()", b.wgsl)};
    case language::msl:
        return {.text = cc::format("threadgroup_barrier(mem_flags::{})", b.msl_memory)};
    }
    return {};
}
} // namespace

void sgl::builtins::register_sync(registry& r)
{
    r.add_comment("// Barriers, which a compute shader reaches in control flow every thread of its workgroup takes "
                  "(CHK-282).");
    for (auto i = u32(0); i < u32(sizeof(k_barriers) / sizeof(k_barriers[0])); ++i)
        r.add(function_record{
            .signature = cc::format("@stages(.compute) fun {}()", k_barriers[i].name),
            .doc = k_barriers[i].doc,
            .evaluate = nothing,
            .write = {.kind = spelling_kind::custom, .custom = write_barrier, .data = i},
            .is_barrier = true,
        });
}
