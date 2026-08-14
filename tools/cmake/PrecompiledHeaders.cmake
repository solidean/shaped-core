# Per-target precompiled headers.
# Included once from the root CMakeLists; it defines two functions and one tier, so including it has almost no effect on its own.
# docs/guides/precompiled-headers.md is the workflow — how to pick a tier, and what the no-PCH presets exist to catch.
#
# A per-target PCH removes roughly half of first-party compile CPU (measured: a semi-cold rebuild of the default preset, 65.0 s -> 34.9 s).
# The restore cost is ~3 ms/MB up to ~28 MB and then flat, because clang deserializes lazily: a TU pays for what it references, not for what the PCH holds.
# So size is not the constraint. Content mismatch is.
# shaped-linter-core goes 0.42 -> 0.50 and nexus-test 0.63 -> 0.84 when the heavy STL block is added, because both are pure clean-core consumers paying to load headers they never read.
# That is why content is tiered per target rather than one repo-wide blob, and why promoting a target's tier is a measurement rather than a guess.
#
# **A library declares its own tiers, in its own CMakeLists.**
# Only the STD tier below is central, because it names standard headers and belongs to no library.
# A list of clean-core headers kept here would be a file nobody editing clean-core ever opens, and it would rot within a release.
#
# Turning it off: CMAKE_DISABLE_PRECOMPILE_HEADERS=ON, CMake's own switch, which makes every target_precompile_headers() a no-op.
# There is deliberately no SC_PCH option — it would implement nothing but a second name for that switch, and two levers can disagree.
# The nopch-* and debug-nopch-* presets set it, and they are the gate that stops /FI from masking a missing include.
#
# **Headers are named in the ANGLE form <a/b.hh>**, which is load-bearing twice over:
#   - it resolves through the CONSUMING TARGET's include directories at compile time, so a tier declared in libs/base/clean-core reaches the same headers when applied to a target in tools/shaped-linter;
#   - a bare "a/b.hh" would have its quotes stripped by CMake and be read as a path relative to the declaring directory, which is the quiet way to get this wrong.
# Use [["a/b.hh"]] if a quoted include is ever genuinely needed.
# No generator expressions are used, so $<ANGLE-R> is not required — but every '>' in an angle entry would terminate a genex, so adding one means switching all of them.

# sc_declare_pch_tier(<NAME> [EXTENDS <tier>...] [HEADERS <header>...])
#
# Registers a PCH tier under a repo-wide name, for any target to apply.
# Declare it in the CMakeLists of the library whose headers it names, next to the target that owns them.
#
# EXTENDS composes tiers: the parents' headers land first, in declared order, then this tier's own.
# Every parent must already be declared, which is what makes a cycle impossible to express and forces the tier graph to follow the library dependency order the root CMakeLists already imposes.
#
# A tier naming a platform SDK gate — DX12, DXC — must be terminal: nothing extends it, and a call site names it last.
# Those gates bracket the SDK headers in `#define byte win_byte_override` and close the bracket themselves, so nothing after them is actually at risk;
# keeping them last is what stops that from becoming load-bearing the day someone adds a header the gate does not close over.
function(sc_declare_pch_tier name)
    cmake_parse_arguments(PARSE_ARGV 1 _t "" "" "EXTENDS;HEADERS")
    if(_t_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "sc_declare_pch_tier(${name}): unexpected argument(s): ${_t_UNPARSED_ARGUMENTS}")
    endif()

    get_property(_exists GLOBAL PROPERTY SC_PCH_TIER_${name}_DEFINED)
    if(_exists)
        # Silently letting the second declaration win would make the applied content depend on add_subdirectory order.
        message(FATAL_ERROR "sc_declare_pch_tier(${name}): already declared")
    endif()

    foreach(parent IN LISTS _t_EXTENDS)
        get_property(_parent_exists GLOBAL PROPERTY SC_PCH_TIER_${parent}_DEFINED)
        if(NOT _parent_exists)
            message(FATAL_ERROR
                "sc_declare_pch_tier(${name}): EXTENDS unknown tier '${parent}'. "
                "A tier must be declared before it is extended, so the library declaring '${parent}' has to be added first in the root CMakeLists.")
        endif()
    endforeach()

    # GLOBAL rather than directory scope: a tier declared in libs/base/clean-core has to be visible in tools/shaped-linter, and directory properties do not travel sideways.
    set_property(GLOBAL PROPERTY SC_PCH_TIER_${name}_DEFINED TRUE)
    set_property(GLOBAL PROPERTY SC_PCH_TIER_${name}_EXTENDS "${_t_EXTENDS}")
    set_property(GLOBAL PROPERTY SC_PCH_TIER_${name}_HEADERS "${_t_HEADERS}")
endfunction()

# The expensive standard headers ranked in docs/notes/build-times.md, pulled in directly rather than through whichever of ours happens to reach them.
# Central because they belong to no library of ours.
# This is the block that HURTS a pure clean-core consumer, which is why it is its own tier rather than part of the clean-core ones.
sc_declare_pch_tier(STD HEADERS
    <chrono> <memory> <mutex> <string> <string_view>
    <system_error> <ranges> <atomic> <type_traits> <utility>
)

# Resolve `ARGN` (tier names) to a deduplicated header list, parents before children, first occurrence winning.
function(_sc_pch_headers out)
    set(_acc "")
    foreach(tier IN LISTS ARGN)
        get_property(_known GLOBAL PROPERTY SC_PCH_TIER_${tier}_DEFINED)
        if(NOT _known)
            # _sc_pch_current is set by sc_finalize_pch and reaches here through CMake's dynamic function scoping, so the message can name the target that asked.
            message(FATAL_ERROR
                "sc_target_pch(${_sc_pch_current}): unknown PCH tier '${tier}'. "
                "Declare it with sc_declare_pch_tier() in the CMakeLists of the library whose headers it names.")
        endif()
        get_property(_parents GLOBAL PROPERTY SC_PCH_TIER_${tier}_EXTENDS)
        if(_parents)
            _sc_pch_headers(_sub ${_parents})
            list(APPEND _acc ${_sub})
        endif()
        get_property(_own GLOBAL PROPERTY SC_PCH_TIER_${tier}_HEADERS)
        list(APPEND _acc ${_own})
    endforeach()
    # Keeps the first occurrence, so a header reached through two tiers stays at its earliest position.
    list(REMOVE_DUPLICATES _acc)
    set(${out} "${_acc}" PARENT_SCOPE)
endfunction()

# sc_target_pch(<target> <tier>...)
#
# Records that a target should be built against one or more tiers, composed left to right.
# Put the call after the target's target_link_libraries, so a reader can check downward that the target actually reaches the tiers' headers — an entry it cannot reach is a hard error in every one of its TUs.
#
# Naming several tiers is how an orthogonal one joins a rung: `sc_target_pch(clean-core-test CC_STD NEXUS)`.
# NEXUS is not inferred from a `-test` suffix, because nexus-test's own best tier is CC *without* nexus.
#
# **Recorded here, applied by sc_finalize_pch() at the end of the root CMakeLists**, so a target may name a tier its own library is added before.
# clean-core-test is exactly that case: it wants NEXUS, and nexus has to be added after clean-core because it depends on it.
# Resolving eagerly would make the legal tier set depend on add_subdirectory order, which is a rule nobody could hold in their head.
function(sc_target_pch target)
    if(NOT TARGET ${target})
        # Strict rather than a silent no-op.
        # Every conditional target here already has its call inside the guard that created it, so reaching this line means a typo —
        # and a typo that silently skipped a PCH would cost that target ~half its compile time with no symptom at all.
        message(FATAL_ERROR "sc_target_pch: no such target '${target}'")
    endif()
    if(NOT ARGN)
        message(FATAL_ERROR "sc_target_pch(${target}): name at least one tier")
    endif()
    # One space-joined "target tier..." record per call; neither a target nor a tier name contains a space.
    string(JOIN " " _record ${target} ${ARGN})
    set_property(GLOBAL APPEND PROPERTY SC_PCH_REQUESTS "${_record}")
endfunction()

# Resolve and apply every sc_target_pch() request. Called once, at the end of the root CMakeLists.
function(sc_finalize_pch)
    get_property(_requests GLOBAL PROPERTY SC_PCH_REQUESTS)
    foreach(record IN LISTS _requests)
        separate_arguments(_parts UNIX_COMMAND "${record}")
        list(POP_FRONT _parts _target)
        set(_sc_pch_current "${_target}") # read by _sc_pch_headers' error path, via dynamic scoping
        _sc_pch_headers(_headers ${_parts})

        # PRIVATE, never PUBLIC.
        # PUBLIC lands the list in INTERFACE_PRECOMPILE_HEADERS and forces it on every consumer's TUs, including a downstream project consuming shaped-core via add_subdirectory.
        # A tier is a measured fact about OUR translation units, not a term of our interface.
        target_precompile_headers(${_target} PRIVATE ${_headers})
    endforeach()
endfunction()
