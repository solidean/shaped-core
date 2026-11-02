#include <clean-core/platform/module_table.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>

cc::string cc::describe_site(source_location const& s)
{
#if CC_SOURCE_SITES == 2
    return cc::format("{}:{} ({})", s.file_name(), s.line(), s.function_name());
#else
    auto const address = u64(reinterpret_cast<uintptr_t>(site_address(s)));
    if (address == 0)
        return {};
    // the module and the offset into it, which is what its debug info is keyed on; a cold path, so the table is not cached
    for (auto const& m : cc::enumerate_loaded_modules())
        if (m.contains(address))
            return cc::format("{}+0x{:x}", m.name(), address - m.base);
    return cc::format("0x{:x}", address);
#endif
}
