include(CheckCXXSourceCompiles)

function(cc_detect_linker out_var)
    # Return cached result immediately if available.
    if(DEFINED ${out_var})
        return()
    endif()

    # MSVC uses its own linker, nothing to detect.
    if(MSVC)
        set(${out_var} "msvc" CACHE INTERNAL "Detected linker (MSVC)")
        return()
    endif()

    set(_src [[
        int main() { return 0; }
    ]])

    set(CMAKE_REQUIRED_QUIET TRUE)

    # 1) mold — fastest available linker
    set(CMAKE_REQUIRED_LINK_OPTIONS -fuse-ld=mold)
    check_cxx_source_compiles("${_src}" _CC_LINKER_MOLD)

    if(_CC_LINKER_MOLD)
        set(${out_var} "mold" CACHE INTERNAL "Detected linker")
        return()
    endif()

    # 2) lld — LLVM's linker, faster than GNU ld/gold
    set(CMAKE_REQUIRED_LINK_OPTIONS -fuse-ld=lld)
    check_cxx_source_compiles("${_src}" _CC_LINKER_LLD)

    if(_CC_LINKER_LLD)
        set(${out_var} "lld" CACHE INTERNAL "Detected linker")
        return()
    endif()

    # 3) System default
    set(${out_var} "" CACHE INTERNAL "Detected linker (system default)")
endfunction()
