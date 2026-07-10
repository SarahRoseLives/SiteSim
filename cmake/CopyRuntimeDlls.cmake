# CopyRuntimeDlls.cmake — resolve an executable's mingw DLL dependency *closure*
# (transitively) with ldd and copy them next to the executable.  Also bundles
# the SoapySDR device modules (HackRF / RTL-SDR) and their own dependency
# closure so SiteSim can talk to SDR hardware when run outside the MSYS2 shell.
# Invoked as a POST_BUILD step via `cmake -P` on Windows.
#
# Required cache/-D arguments:
#   EXE          full path to the built executable
#   DEST_DIR     directory to copy the DLLs into
#   MINGW_BIN    mingw64 bin directory (source of the DLLs, by basename)
#   LDD          path to the ldd executable
# Optional:
#   SOAPY_MODULE_DIR  mingw64 SoapySDR modules dir (e.g. .../lib/SoapySDR/modules0.8)
#   EXTRA_DLL_DIRS    extra ;-separated dirs to search for DLLs by basename
#                     (e.g. the superbuild vendor prefix that holds libmbe-neo.dll)

if(NOT EXE OR NOT DEST_DIR OR NOT MINGW_BIN OR NOT LDD)
    message(FATAL_ERROR "CopyRuntimeDlls.cmake: EXE, DEST_DIR, MINGW_BIN and LDD are required")
endif()

# All directories to search for a DLL by basename, in priority order.
set(_SEARCH_DIRS "${MINGW_BIN}")
if(EXTRA_DLL_DIRS)
    list(APPEND _SEARCH_DIRS ${EXTRA_DLL_DIRS})
endif()

# Resolve a DLL basename to a full path across the search dirs. Empty if none.
function(resolve_dll name out_var)
    foreach(_dir IN LISTS _SEARCH_DIRS)
        if(EXISTS "${_dir}/${name}")
            set(${out_var} "${_dir}/${name}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# Return the basenames of the mingw/vendor DLLs that `ldd <binary>` reports.
function(ldd_direct_deps binary out_var)
    execute_process(
        COMMAND "${LDD}" "${binary}"
        OUTPUT_VARIABLE _ldd_out
        RESULT_VARIABLE _ldd_res
    )
    set(_names "")
    if(_ldd_res EQUAL 0)
        # Match resolved paths under a mingw/vendor prefix, e.g.
        #   SDL2.dll => /mingw64/bin/SDL2.dll (0x...)
        #   libmbe-neo.dll => /.../vendor-prefix/bin/libmbe-neo.dll (0x...)
        string(REGEX MATCHALL "=> [^\n]*\\.dll" _matches "${_ldd_out}")
        foreach(_m IN LISTS _matches)
            string(REGEX REPLACE "^=> " "" _path "${_m}")
            get_filename_component(_name "${_path}" NAME)
            list(APPEND _names "${_name}")
        endforeach()
        # Also capture entries ldd cannot resolve in its own environment, e.g.
        #   libmbe-neo.dll => not found
        # These often live in EXTRA_DLL_DIRS (the superbuild vendor prefix).
        string(REGEX MATCHALL "[A-Za-z0-9._+-]+\\.dll => not found" _missing "${_ldd_out}")
        foreach(_m IN LISTS _missing)
            string(REGEX REPLACE " => not found$" "" _name "${_m}")
            list(APPEND _names "${_name}")
        endforeach()
    endif()
    set(${out_var} "${_names}" PARENT_SCOPE)
endfunction()

# Copy the full transitive DLL closure of every binary in <seeds> into <dest>.
# Only DLLs found in the search dirs are copied (system DLLs are skipped).
function(copy_mingw_closure dest)
    set(_worklist ${ARGN})    # initial binaries to inspect
    set(_seen "")             # DLL basenames already handled
    while(_worklist)
        list(POP_FRONT _worklist _bin)
        ldd_direct_deps("${_bin}" _deps)
        foreach(_name IN LISTS _deps)
            if(NOT _name IN_LIST _seen)
                list(APPEND _seen "${_name}")
                resolve_dll("${_name}" _src)
                if(_src)
                    # COPY_FILE (not COPY) skips setting the mtime, which races
                    # when the sitesim and uplink_test post-build steps copy the
                    # same DLL into a shared directory in parallel.
                    file(COPY_FILE "${_src}" "${dest}/${_name}"
                         ONLY_IF_DIFFERENT INPUT_MAY_BE_RECENT)
                    # Recurse: this DLL may drag in further dependencies.
                    list(APPEND _worklist "${_src}")
                endif()
            endif()
        endforeach()
    endwhile()
endfunction()

# Copy the full transitive mingw DLL closure of every binary in <seeds> into
# <dest>.
# 1. The executable's own dependency closure, next to the .exe.
copy_mingw_closure("${DEST_DIR}" "${EXE}")

# 2. SoapySDR device modules + their dependency closure.
#    Modules go in <dest>/SoapySDR/modules0.8 (SoapyTx points
#    SOAPY_SDR_PLUGIN_PATH here at runtime); their extra DLL deps (libhackrf,
#    librtlsdr, libusb, ...) go next to the .exe so the loader finds them.
if(SOAPY_MODULE_DIR AND EXISTS "${SOAPY_MODULE_DIR}")
    set(_mod_dest "${DEST_DIR}/SoapySDR/modules0.8")
    file(MAKE_DIRECTORY "${_mod_dest}")
    file(GLOB _modules "${SOAPY_MODULE_DIR}/*.dll")
    set(_seed_modules "")
    foreach(_mod IN LISTS _modules)
        get_filename_component(_modname "${_mod}" NAME)
        file(COPY_FILE "${_mod}" "${_mod_dest}/${_modname}"
             ONLY_IF_DIFFERENT INPUT_MAY_BE_RECENT)
        list(APPEND _seed_modules "${_mod}")
    endforeach()
    if(_seed_modules)
        copy_mingw_closure("${DEST_DIR}" ${_seed_modules})
    endif()
endif()
