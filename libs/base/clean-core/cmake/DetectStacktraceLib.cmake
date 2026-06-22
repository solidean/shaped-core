include(CheckCXXSourceCompiles)

function(cc_detect_stacktrace_link out_var)
    # Return cached result immediately if available.
    if(DEFINED ${out_var})
        return()
    endif()

    set(_src [[
        #include <stacktrace>
        int main() {
            auto st = std::stacktrace::current();
            return (int)st.size();
        }
    ]])

    set(CMAKE_REQUIRED_QUIET TRUE)

    # 1) No extra library needed (MSVC, libc++, newer toolchains)
    unset(CMAKE_REQUIRED_LIBRARIES)
    check_cxx_source_compiles("${_src}" _CC_ST_NO_EXTRA)

    if(_CC_ST_NO_EXTRA)
        set(${out_var} "" CACHE INTERNAL "Stacktrace link library (none needed)")
        return()
    endif()

    # 2) GCC 14+ libstdc++ ships std::stacktrace in -lstdc++exp
    set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
    check_cxx_source_compiles("${_src}" _CC_ST_STDCXXEXP)

    if(_CC_ST_STDCXXEXP)
        set(${out_var} "stdc++exp" CACHE INTERNAL "Stacktrace link library")
        return()
    endif()

    # 3) GCC 13 libstdc++ ships std::stacktrace in -lstdc++_libbacktrace
    set(CMAKE_REQUIRED_LIBRARIES stdc++_libbacktrace)
    check_cxx_source_compiles("${_src}" _CC_ST_LIBBACKTRACE)

    if(_CC_ST_LIBBACKTRACE)
        set(${out_var} "stdc++_libbacktrace" CACHE INTERNAL "Stacktrace link library")
        return()
    endif()

    set(${out_var} "NOTFOUND" CACHE INTERNAL "Stacktrace link library (not found)")
endfunction()
