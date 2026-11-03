include_guard(GLOBAL)

# ============================================================================
# hlog CMake integration
#
# Expected repository layout:
#
#   <project>/lib/
#   ├── hlog/
#   │   ├── Cargo.toml
#   │   ├── Cargo.lock
#   │   ├── include/
#   │   │   ├── hlog.h
#   │   │   └── hlog.hpp
#   │   ├── lib/
#   │   │   ├── libhlog.so
#   │   │   ├── libhlog.so.1
#   │   │   └── libhlog.so.1.0.0
#   │   ├── runtime/
#   │   │   ├── Cargo.toml
#   │   │   ├── build.rs
#   │   │   └── src/
#   │   ├── scripts/
#   │   │   ├── install.sh
#   │   │   └── verify_runtime.sh
#   │   └── cmake/
#   │       └── HLog.cmake
#   └── spdlog/
#       └── include/
#           └── spdlog/
#               └── fmt/
#
# Consumer usage:
#
#   include("/project/lib/hlog/cmake/HLog.cmake")
#   target_link_libraries(my_model PRIVATE hlog::hlog)
#
# C-only consumers may link hlog::runtime directly.
# ============================================================================

# ============================================================================
# Resolve paths automatically
# ============================================================================
get_filename_component(
    HLOG_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.."
    ABSOLUTE
)
get_filename_component(
    HLOG_PROJECT_LIB_DIR
    "${HLOG_ROOT}/.."
    ABSOLUTE
)

set(HLOG_INCLUDE_DIR
    "${HLOG_ROOT}/include"
)
set(HLOG_LIB_DIR
    "${HLOG_ROOT}/lib"
)
set(HLOG_LIBRARY
    "${HLOG_LIB_DIR}/libhlog.so"
)
set(HLOG_LIBRARY_SONAME
    "${HLOG_LIB_DIR}/libhlog.so.1"
)
set(HLOG_INSTALL_SCRIPT
    "${HLOG_ROOT}/scripts/install.sh"
)
set(HLOG_VERIFY_SCRIPT
    "${HLOG_ROOT}/scripts/verify_runtime.sh"
)
set(HLOG_CARGO_TARGET_DIR
    "${HLOG_ROOT}/target"
)

# Incremental-build state belongs to the consumer's CMake build tree, not the
# hlog source tree.
set(HLOG_RUNTIME_STATE_DIR
    "${CMAKE_BINARY_DIR}/hlog"
)
set(HLOG_RUNTIME_STAMP
    "${HLOG_RUNTIME_STATE_DIR}/runtime.stamp"
)
file(MAKE_DIRECTORY
    "${HLOG_RUNTIME_STATE_DIR}"
)

# ============================================================================
# Project-local spdlog bundled fmt
# ============================================================================
set(HLOG_SPDLOG_ROOT
    "${HLOG_PROJECT_LIB_DIR}/spdlog"
)
set(HLOG_SPDLOG_INCLUDE_DIR
    "${HLOG_SPDLOG_ROOT}/include"
)
set(HLOG_SPDLOG_FMT_HEADER
    "${HLOG_SPDLOG_INCLUDE_DIR}/spdlog/fmt/fmt.h"
)

# ============================================================================
# Validate source layout
# ============================================================================
if(NOT EXISTS "${HLOG_INCLUDE_DIR}/hlog.h")
    message(FATAL_ERROR
        "hlog: missing public C header:\n"
        "  ${HLOG_INCLUDE_DIR}/hlog.h"
    )
endif()

if(NOT EXISTS "${HLOG_INCLUDE_DIR}/hlog.hpp")
    message(FATAL_ERROR
        "hlog: missing public C++ header:\n"
        "  ${HLOG_INCLUDE_DIR}/hlog.hpp"
    )
endif()

if(NOT EXISTS "${HLOG_SPDLOG_FMT_HEADER}")
    message(FATAL_ERROR
        "hlog: project-local spdlog bundled fmt was not found.\n"
        "\n"
        "Expected:\n"
        "  ${HLOG_SPDLOG_FMT_HEADER}\n"
        "\n"
        "Expected repository layout:\n"
        "  lib/hlog\n"
        "  lib/spdlog"
    )
endif()

if(NOT EXISTS "${HLOG_ROOT}/Cargo.toml")
    message(FATAL_ERROR
        "hlog: workspace Cargo.toml was not found:\n"
        "  ${HLOG_ROOT}/Cargo.toml"
    )
endif()

if(NOT EXISTS "${HLOG_ROOT}/runtime/Cargo.toml")
    message(FATAL_ERROR
        "hlog: runtime Cargo.toml was not found:\n"
        "  ${HLOG_ROOT}/runtime/Cargo.toml"
    )
endif()

if(NOT EXISTS "${HLOG_INSTALL_SCRIPT}")
    message(FATAL_ERROR
        "hlog: install script not found:\n"
        "  ${HLOG_INSTALL_SCRIPT}"
    )
endif()

if(NOT EXISTS "${HLOG_VERIFY_SCRIPT}")
    message(FATAL_ERROR
        "hlog: runtime verification script not found:\n"
        "  ${HLOG_VERIFY_SCRIPT}"
    )
endif()

# ============================================================================
# Runtime build inputs
#
# CONFIGURE_DEPENDS makes CMake rescan runtime/src when Rust files are added or
# removed. Cargo target directories are deliberately outside the glob.
# ============================================================================
file(
    GLOB_RECURSE
    HLOG_RUNTIME_RUST_SOURCES
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    "${HLOG_ROOT}/runtime/src/*.rs"
)

set(HLOG_RUNTIME_INPUT_CANDIDATES
    "${HLOG_ROOT}/Cargo.toml"
    "${HLOG_ROOT}/Cargo.lock"
    "${HLOG_ROOT}/rust-toolchain"
    "${HLOG_ROOT}/rust-toolchain.toml"
    "${HLOG_ROOT}/.cargo/config"
    "${HLOG_ROOT}/.cargo/config.toml"
    "${HLOG_ROOT}/include/hlog.h"
    "${HLOG_ROOT}/runtime/Cargo.toml"
    "${HLOG_ROOT}/runtime/Cargo.lock"
    "${HLOG_ROOT}/runtime/build.rs"
    "${HLOG_INSTALL_SCRIPT}"
    "${HLOG_VERIFY_SCRIPT}"
)

set(HLOG_RUNTIME_INPUTS
    ${HLOG_RUNTIME_RUST_SOURCES}
)
foreach(
    HLOG_RUNTIME_INPUT_CANDIDATE
    IN LISTS HLOG_RUNTIME_INPUT_CANDIDATES
)
    if(EXISTS "${HLOG_RUNTIME_INPUT_CANDIDATE}")
        list(APPEND
            HLOG_RUNTIME_INPUTS
            "${HLOG_RUNTIME_INPUT_CANDIDATE}"
        )
    endif()
endforeach()
list(REMOVE_DUPLICATES
    HLOG_RUNTIME_INPUTS
)

# ============================================================================
# Configure-time runtime preparation
#
# Imported targets need a real runtime file during configure. Build it when it
# is missing, older than a known input, or present but incompatible with ABI v3.
# ============================================================================
function(_hlog_ensure_runtime)
    set(_HLOG_RUNTIME_STALE FALSE)

    if(EXISTS "${HLOG_LIBRARY}" AND
       EXISTS "${HLOG_LIBRARY_SONAME}")
        foreach(
            _HLOG_RUNTIME_INPUT
            IN LISTS HLOG_RUNTIME_INPUTS
        )
            if("${_HLOG_RUNTIME_INPUT}"
               IS_NEWER_THAN
               "${HLOG_LIBRARY_SONAME}")
                set(_HLOG_RUNTIME_STALE TRUE)
                message(STATUS
                    "hlog: runtime input is newer: "
                    "${_HLOG_RUNTIME_INPUT}"
                )
                break()
            endif()
        endforeach()

        if(NOT _HLOG_RUNTIME_STALE)
            execute_process(
                COMMAND
                    bash
                    "${HLOG_VERIFY_SCRIPT}"
                    "${HLOG_LIBRARY_SONAME}"
                RESULT_VARIABLE
                    _HLOG_VERIFY_RESULT
                OUTPUT_VARIABLE
                    _HLOG_VERIFY_STDOUT
                ERROR_VARIABLE
                    _HLOG_VERIFY_STDERR
            )

            if(_HLOG_VERIFY_RESULT EQUAL 0)
                message(STATUS
                    "hlog: using up-to-date ABI v3 runtime: "
                    "${HLOG_LIBRARY}"
                )
                file(TOUCH
                    "${HLOG_RUNTIME_STAMP}"
                )
                return()
            endif()

            set(_HLOG_RUNTIME_STALE TRUE)
            message(STATUS
                "hlog: installed runtime failed ABI v3 verification; rebuilding"
            )
        endif()

        message(STATUS
            "hlog: rebuilding stale runtime: "
            "${HLOG_LIBRARY}"
        )
    else()
        message(STATUS
            "hlog: runtime is incomplete or missing: "
            "${HLOG_LIBRARY}"
        )
    endif()

    message(STATUS
        "hlog: running install script: "
        "${HLOG_INSTALL_SCRIPT}"
    )
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${HLOG_CARGO_TARGET_DIR}"
            bash
            "${HLOG_INSTALL_SCRIPT}"
            "${HLOG_ROOT}"
        WORKING_DIRECTORY
            "${HLOG_ROOT}"
        RESULT_VARIABLE
            _HLOG_INSTALL_RESULT
        OUTPUT_VARIABLE
            _HLOG_INSTALL_STDOUT
        ERROR_VARIABLE
            _HLOG_INSTALL_STDERR
    )

    if(NOT _HLOG_INSTALL_RESULT EQUAL 0)
        message(FATAL_ERROR
            "hlog: installation failed.\n"
            "\n"
            "script:\n"
            "  ${HLOG_INSTALL_SCRIPT}\n"
            "\n"
            "exit code:\n"
            "  ${_HLOG_INSTALL_RESULT}\n"
            "\n"
            "stdout:\n"
            "${_HLOG_INSTALL_STDOUT}\n"
            "\n"
            "stderr:\n"
            "${_HLOG_INSTALL_STDERR}"
        )
    endif()

    if(NOT EXISTS "${HLOG_LIBRARY}")
        message(FATAL_ERROR
            "hlog: install.sh completed successfully, but the runtime library is still missing:\n"
            "  ${HLOG_LIBRARY}"
        )
    endif()

    if(NOT EXISTS "${HLOG_LIBRARY_SONAME}")
        message(FATAL_ERROR
            "hlog: SONAME library is missing:\n"
            "  ${HLOG_LIBRARY_SONAME}"
        )
    endif()

    execute_process(
        COMMAND
            bash
            "${HLOG_VERIFY_SCRIPT}"
            "${HLOG_LIBRARY_SONAME}"
        RESULT_VARIABLE
            _HLOG_VERIFY_RESULT
        OUTPUT_VARIABLE
            _HLOG_VERIFY_STDOUT
        ERROR_VARIABLE
            _HLOG_VERIFY_STDERR
    )
    if(NOT _HLOG_VERIFY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "hlog: installed runtime failed ABI v3 verification.\n"
            "\n"
            "stdout:\n"
            "${_HLOG_VERIFY_STDOUT}\n"
            "\n"
            "stderr:\n"
            "${_HLOG_VERIFY_STDERR}"
        )
    endif()

    file(TOUCH
        "${HLOG_RUNTIME_STAMP}"
    )
    message(STATUS
        "hlog: runtime installed successfully: "
        "${HLOG_LIBRARY}"
    )
endfunction()

_hlog_ensure_runtime()

# ============================================================================
# Build-time incremental runtime rebuild
#
# Unlike execute_process(), this rule is part of Make/Ninja's dependency graph.
# Editing, adding, or removing runtime Rust inputs therefore rebuilds and
# verifies libhlog before dependent model targets are relinked.
# ============================================================================
add_custom_command(
    OUTPUT
        "${HLOG_RUNTIME_STAMP}"
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "CARGO_TARGET_DIR=${HLOG_CARGO_TARGET_DIR}"
        bash
        "${HLOG_INSTALL_SCRIPT}"
        "${HLOG_ROOT}"
    COMMAND
        bash
        "${HLOG_VERIFY_SCRIPT}"
        "${HLOG_LIBRARY_SONAME}"
    COMMAND
        "${CMAKE_COMMAND}" -E touch
        "${HLOG_RUNTIME_STAMP}"
    DEPENDS
        ${HLOG_RUNTIME_INPUTS}
    BYPRODUCTS
        "${HLOG_LIBRARY}"
        "${HLOG_LIBRARY_SONAME}"
    WORKING_DIRECTORY
        "${HLOG_ROOT}"
    COMMENT
        "hlog: rebuilding changed ABI v3 runtime"
    VERBATIM
    USES_TERMINAL
)

add_custom_target(
    hlog_runtime_build
    DEPENDS
        "${HLOG_RUNTIME_STAMP}"
)

# ============================================================================
# Imported runtime target
# ============================================================================
if(NOT TARGET hlog::runtime)
    add_library(
        hlog::runtime
        SHARED
        IMPORTED
        GLOBAL
    )
    set_target_properties(
        hlog::runtime
        PROPERTIES
            IMPORTED_LOCATION
                "${HLOG_LIBRARY_SONAME}"
            IMPORTED_SONAME
                "libhlog.so.1"
            INTERFACE_INCLUDE_DIRECTORIES
                "${HLOG_INCLUDE_DIR};${HLOG_SPDLOG_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS
                "HLOG_USE_SPDLOG_FMT=1"
    )
endif()

add_dependencies(
    hlog::runtime
    hlog_runtime_build
)

# ============================================================================
# Public C++ interface target
# ============================================================================
if(NOT TARGET hlog::hlog)
    add_library(
        hlog::hlog
        INTERFACE
        IMPORTED
        GLOBAL
    )
    set_target_properties(
        hlog::hlog
        PROPERTIES
            INTERFACE_LINK_LIBRARIES
                "hlog::runtime"
            INTERFACE_INCLUDE_DIRECTORIES
                "${HLOG_INCLUDE_DIR};${HLOG_SPDLOG_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS
                "HLOG_USE_SPDLOG_FMT=1"
            INTERFACE_COMPILE_FEATURES
                "cxx_std_17"
    )
endif()

add_dependencies(
    hlog::hlog
    hlog_runtime_build
)

# ============================================================================
# Diagnostics
# ============================================================================
message(STATUS
    "hlog root: ${HLOG_ROOT}"
)
message(STATUS
    "hlog runtime: ${HLOG_LIBRARY_SONAME}"
)
message(STATUS
    "hlog spdlog: ${HLOG_SPDLOG_ROOT}"
)
message(STATUS
    "hlog runtime stamp: ${HLOG_RUNTIME_STAMP}"
)
