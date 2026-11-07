#
# PROJECT:     LiberNT - Leeway Development Works
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Leeway Build Identity (LBI) generator - the one place that turns an
#              allocated build attempt into lbi.h (kernel, user mode, .rc) and
#              lbi.json (diagnostics, provenance manifest).
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
# WS-B prototype, spec: shared/libernt/lbi/WS-B-LBI-SPEC.md section 6.
#
# Inputs
#   LBI_ALLOCATION_FILE  lbi-allocation.cmake written by sdk/tools/lbi/lbi_alloc.py
#                        before configure (default: ${REACTOS_BINARY_DIR}/lbi-allocation.cmake).
#                        Absent -> provisional identity (sequence 0, random attempt id).
#   LBI_CHANNEL          channel for a provisional identity (default ldw_dev)
#   LBI_REVISION         servicing revision for a provisional identity (default 0)
#   WINARCH, DBG, CMAKE_C_COMPILER_ID/VERSION, COMMIT_HASH (from version.cmake when present)
#
# Outputs
#   ${REACTOS_BINARY_DIR}/sdk/include/reactos/lbi.h
#   ${REACTOS_BINARY_DIR}/lbi.json
#
# Script mode (tests): cmake -DLBI_SCRIPT_MODE=1 -DREACTOS_SOURCE_DIR=.. -DREACTOS_BINARY_DIR=.. -P lbi.cmake
#
# Nothing here touches the NT compatibility identity (ntverp.h, NtBuildNumber,
# CurrentVersion, UBR): LBI is domain B of the owner directive, domain A stays
# with WS-A.

if(NOT DEFINED REACTOS_SOURCE_DIR OR NOT DEFINED REACTOS_BINARY_DIR)
    message(FATAL_ERROR "lbi.cmake needs REACTOS_SOURCE_DIR and REACTOS_BINARY_DIR")
endif()
if(NOT DEFINED LBI_SOURCE_DIR)
    set(LBI_SOURCE_DIR "${REACTOS_SOURCE_DIR}")
endif()
if(NOT DEFINED LBI_ALLOCATION_FILE)
    set(LBI_ALLOCATION_FILE "${REACTOS_BINARY_DIR}/lbi-allocation.cmake")
endif()
if(NOT DEFINED LBI_CHANNEL)
    set(LBI_CHANNEL "ldw_dev")
endif()
if(NOT DEFINED LBI_REVISION)
    set(LBI_REVISION 0)
endif()
if(NOT DEFINED LBI_MANIFEST_SCHEMA)
    # WS-C owns this identifier (cross-dependency, spec section 8).
    set(LBI_MANIFEST_SCHEMA "leeway-provenance-manifest/v1")
endif()

# ---- arch / type from the configured tree ----------------------------------
if(NOT DEFINED WINARCH)
    message(FATAL_ERROR "lbi.cmake: WINARCH is not set")
endif()
set(LBI_ARCH "${WINARCH}")
if(NOT LBI_ARCH MATCHES "^(amd64|x86|arm64|riscv64|ppc)$")
    message(FATAL_ERROR "lbi.cmake: arch '${LBI_ARCH}' is not in the LBI grammar")
endif()
if(DBG)
    set(LBI_TYPE "chk")
else()
    set(LBI_TYPE "fre")
endif()

# ---- source commit / dirty flag ----------------------------------------------
set(LBI_SOURCE_COMMIT "unknown")
set(LBI_SOURCE_DIRTY 0)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
                    WORKING_DIRECTORY "${LBI_SOURCE_DIR}"
                    OUTPUT_VARIABLE _lbi_commit RESULT_VARIABLE _lbi_rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_lbi_rc EQUAL 0 AND _lbi_commit MATCHES "^[0-9a-f]+$")
        set(LBI_SOURCE_COMMIT "${_lbi_commit}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                    WORKING_DIRECTORY "${LBI_SOURCE_DIR}"
                    OUTPUT_VARIABLE _lbi_porcelain RESULT_VARIABLE _lbi_rc ERROR_QUIET)
    if(_lbi_rc EQUAL 0 AND NOT "${_lbi_porcelain}" STREQUAL "")
        set(LBI_SOURCE_DIRTY 1)
    endif()
endif()
if(LBI_SOURCE_COMMIT STREQUAL "unknown" AND DEFINED COMMIT_HASH AND NOT COMMIT_HASH STREQUAL "unknown-hash")
    set(LBI_SOURCE_COMMIT "${COMMIT_HASH}")
endif()

# ---- allocation (ledger) or provisional ---------------------------------------
if(EXISTS "${LBI_ALLOCATION_FILE}")
    include("${LBI_ALLOCATION_FILE}")
    if(NOT LBI_ALLOC_ARCH STREQUAL LBI_ARCH OR NOT LBI_ALLOC_TYPE STREQUAL LBI_TYPE)
        message(FATAL_ERROR "lbi.cmake: allocation is for ${LBI_ALLOC_ARCH}${LBI_ALLOC_TYPE}, tree is ${LBI_ARCH}${LBI_TYPE}")
    endif()
    if(NOT LBI_ALLOC_SOURCE_COMMIT STREQUAL LBI_SOURCE_COMMIT)
        message(FATAL_ERROR "lbi.cmake: allocation was made for commit ${LBI_ALLOC_SOURCE_COMMIT}, tree is at ${LBI_SOURCE_COMMIT}; allocate again")
    endif()
    set(LBI_SEQUENCE "${LBI_ALLOC_SEQUENCE}")
    set(LBI_REVISION "${LBI_ALLOC_REVISION}")
    set(LBI_CHANNEL "${LBI_ALLOC_CHANNEL}")
    set(LBI_TIMESTAMP_UTC "${LBI_ALLOC_TIMESTAMP_UTC}")
    set(LBI_STAMP "${LBI_ALLOC_STAMP}")
    set(LBI_ATTEMPT_ID "${LBI_ALLOC_ATTEMPT_ID}")
    set(LBI_MANIFEST_REF "${LBI_ALLOC_MANIFEST_REF}")
    set(LBI_WORKER "${LBI_ALLOC_WORKER}")
    set(LBI_CI_REF "${LBI_ALLOC_CI_REF}")
    set(LBI_PROVISIONAL "${LBI_ALLOC_PROVISIONAL}")
    if(LBI_ALLOC_SOURCE_DIRTY)
        set(LBI_SOURCE_DIRTY 1)
    endif()
    file(TO_CMAKE_PATH "${LBI_ALLOCATION_FILE}" LBI_ALLOCATION_SOURCE)
else()
    set(LBI_SEQUENCE 0)
    set(LBI_PROVISIONAL 1)
    string(TIMESTAMP LBI_TIMESTAMP_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
    string(TIMESTAMP LBI_STAMP "%y%m%d-%H%M" UTC)
    string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef _lbi_rand)
    string(SUBSTRING "${_lbi_rand}" 0 8 _a)
    string(SUBSTRING "${_lbi_rand}" 8 4 _b)
    string(SUBSTRING "${_lbi_rand}" 12 4 _c)
    string(SUBSTRING "${_lbi_rand}" 16 4 _d)
    string(SUBSTRING "${_lbi_rand}" 20 12 _e)
    set(LBI_ATTEMPT_ID "${_a}-${_b}-${_c}-${_d}-${_e}")
    set(LBI_MANIFEST_REF "${LBI_MANIFEST_SCHEMA}:attempt:${LBI_ATTEMPT_ID}")
    if(DEFINED ENV{LBI_WORKER})
        set(LBI_WORKER "$ENV{LBI_WORKER}")
    else()
        cmake_host_system_information(RESULT LBI_WORKER QUERY HOSTNAME)
    endif()
    set(LBI_CI_REF "$ENV{GITHUB_RUN_ID}")
    set(LBI_ALLOCATION_SOURCE "provisional")
endif()
# JSON safety for the two free-text fields (worker names and paths may carry odd characters)
foreach(_v LBI_WORKER LBI_ALLOCATION_SOURCE)
    string(REPLACE "\\" "/" ${_v} "${${_v}}")
    string(REPLACE "\"" "'" ${_v} "${${_v}}")
endforeach()

if(NOT LBI_CHANNEL MATCHES "^[a-z][a-z0-9_]*$")
    message(FATAL_ERROR "lbi.cmake: channel '${LBI_CHANNEL}' is not in the LBI grammar")
endif()
string(LENGTH "${LBI_CHANNEL}" _lbi_chlen)
if(_lbi_chlen GREATER 32)
    message(FATAL_ERROR "lbi.cmake: channel '${LBI_CHANNEL}' is longer than 32")
endif()
if(NOT LBI_SEQUENCE MATCHES "^[0-9]+$" OR NOT LBI_REVISION MATCHES "^[0-9]+$")
    message(FATAL_ERROR "lbi.cmake: sequence/revision must be decimal")
endif()
if(NOT LBI_STAMP MATCHES "^[0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9][0-9][0-9]$")
    message(FATAL_ERROR "lbi.cmake: stamp '${LBI_STAMP}' is not yymmdd-hhmm")
endif()

# ---- the three identity strings (grammar: sdk/tools/lbi/lbi_format.py) -----------
set(LBI_LABEL      "${LBI_SEQUENCE}.${LBI_REVISION}.${LBI_ARCH}${LBI_TYPE}.${LBI_CHANNEL}.${LBI_STAMP}")
set(LBI_BUILDLAB   "${LBI_SEQUENCE}.${LBI_CHANNEL}.${LBI_STAMP}")
set(LBI_BUILDLABEX "${LBI_LABEL}")

# ---- reproducible-inputs digest (spec section 4) -----------------------------------
# Same canonical JSON as lbi_format.inputs_digest(): keys sorted, no spaces.
set(_lbi_cfg_items "")
foreach(_v DBG KDBG _WINKD CMAKE_BUILD_TYPE ROSCONFIG_PROFILE ENABLE_DXILCONV ENABLE_DXCOMPILER_L2 LBI_SCRIPT_MODE)
    if(DEFINED ${_v} AND NOT _v STREQUAL "LBI_SCRIPT_MODE")
        list(APPEND _lbi_cfg_items "${_v}=${${_v}}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _lbi_cfg_items)
list(SORT _lbi_cfg_items)
set(_lbi_cfg_json "")
foreach(_i IN LISTS _lbi_cfg_items)
    if(NOT _lbi_cfg_json STREQUAL "")
        string(APPEND _lbi_cfg_json ",")
    endif()
    string(APPEND _lbi_cfg_json "\"${_i}\"")
endforeach()
if(LBI_SOURCE_DIRTY)
    set(_lbi_dirty_json "true")
else()
    set(_lbi_dirty_json "false")
endif()
if(NOT DEFINED CMAKE_C_COMPILER_ID)
    set(CMAKE_C_COMPILER_ID "unknown")
endif()
if(NOT DEFINED CMAKE_C_COMPILER_VERSION)
    set(CMAKE_C_COMPILER_VERSION "unknown")
endif()
set(_lbi_canon "{\"arch\":\"${LBI_ARCH}\",\"channel\":\"${LBI_CHANNEL}\",\"config\":[${_lbi_cfg_json}],\"manifest_schema\":\"${LBI_MANIFEST_SCHEMA}\",\"source_commit\":\"${LBI_SOURCE_COMMIT}\",\"source_dirty\":${_lbi_dirty_json},\"toolchain\":[\"${CMAKE_C_COMPILER_ID}\",\"${CMAKE_C_COMPILER_VERSION}\"],\"type\":\"${LBI_TYPE}\",\"v\":1}")
string(SHA256 LBI_INPUTS_DIGEST "${_lbi_canon}")
string(SUBSTRING "${LBI_INPUTS_DIGEST}" 0 16 LBI_INPUTS_DIGEST_SHORT)

# ---- outputs ---------------------------------------------------------------------
configure_file("${REACTOS_SOURCE_DIR}/sdk/include/reactos/lbi.h.cmake"
               "${REACTOS_BINARY_DIR}/sdk/include/reactos/lbi.h")

set(_lbi_json "{
  \"lbi_version\": 1,
  \"label\": \"${LBI_LABEL}\",
  \"buildlab\": \"${LBI_BUILDLAB}\",
  \"buildlabex\": \"${LBI_BUILDLABEX}\",
  \"attempt_id\": \"${LBI_ATTEMPT_ID}\",
  \"sequence\": ${LBI_SEQUENCE},
  \"revision\": ${LBI_REVISION},
  \"arch\": \"${LBI_ARCH}\",
  \"type\": \"${LBI_TYPE}\",
  \"channel\": \"${LBI_CHANNEL}\",
  \"timestamp_utc\": \"${LBI_TIMESTAMP_UTC}\",
  \"stamp\": \"${LBI_STAMP}\",
  \"source_commit\": \"${LBI_SOURCE_COMMIT}\",
  \"source_dirty\": ${_lbi_dirty_json},
  \"manifest_schema\": \"${LBI_MANIFEST_SCHEMA}\",
  \"manifest_ref\": \"${LBI_MANIFEST_REF}\",
  \"inputs_digest\": \"${LBI_INPUTS_DIGEST}\",
  \"toolchain\": {\"id\": \"${CMAKE_C_COMPILER_ID}\", \"version\": \"${CMAKE_C_COMPILER_VERSION}\"},
  \"config\": [${_lbi_cfg_json}],
  \"worker\": \"${LBI_WORKER}\",
  \"ci_ref\": \"${LBI_CI_REF}\",
  \"provisional\": ${LBI_PROVISIONAL},
  \"allocation_source\": \"${LBI_ALLOCATION_SOURCE}\"
}
")
string(REPLACE "\"provisional\": 1" "\"provisional\": true" _lbi_json "${_lbi_json}")
string(REPLACE "\"provisional\": 0" "\"provisional\": false" _lbi_json "${_lbi_json}")
file(WRITE "${REACTOS_BINARY_DIR}/lbi.json" "${_lbi_json}")

if(NOT LBI_SCRIPT_MODE)
    message(STATUS "LBI: ${LBI_LABEL} (attempt ${LBI_ATTEMPT_ID}, inputs ${LBI_INPUTS_DIGEST_SHORT}, ${LBI_ALLOCATION_SOURCE})")
    if(LBI_PROVISIONAL)
        message(STATUS "LBI: provisional identity (sequence 0) - run sdk/tools/lbi/lbi_alloc.py --ledger ... before configure for an allocated one")
    endif()
endif()
