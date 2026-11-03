#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(
    cd "$(dirname "${BASH_SOURCE[0]}")"
    pwd
)"
ROOT_DIR="$(
    cd "${SCRIPT_DIR}/.."
    pwd
)"

#
# Default install location is the hlog source root itself:
#
#   lib/hlog/
#
# Therefore:
#
#   lib/
#   include/
#
# live directly under ROOT_DIR.
#
PREFIX="${1:-${ROOT_DIR}}"

TARGET_DIR="${CARGO_TARGET_DIR:-${ROOT_DIR}/target}"

# A device crate may invoke this script from its own build.rs. Cargo then
# exports a jobserver through CARGO_MAKEFLAGS. Starting another Cargo process
# with those inherited descriptors can fail with an early EOF on the jobserver
# pipe. The nested build gets its own scheduler instead. One job is the safe
# default; callers may raise it with HLOG_BUILD_JOBS.
BUILD_JOBS="${HLOG_BUILD_JOBS:-1}"

if [[ ! "${BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "ERROR: HLOG_BUILD_JOBS must be a positive integer" >&2
    exit 1
fi

RUNTIME_PACKAGE="hlog-runtime"
SO_NAME="libhlog.so"
SO_SONAME="libhlog.so.1"
SO_VERSIONED="libhlog.so.1.0.0"

INCLUDE_SRC="${ROOT_DIR}/include"
LIB_DST="${PREFIX}/lib"
INCLUDE_DST="${PREFIX}/include"
BUILT_SO="${TARGET_DIR}/release/${SO_NAME}"

# Multiple model crates can notice the same source change and invoke this
# installer concurrently. Serialize the complete build/install transaction.
mkdir -p "${TARGET_DIR}"
LOCK_FILE="${TARGET_DIR}/.hlog-install.lock"
exec 9>"${LOCK_FILE}"
flock 9

echo "============================================================"
echo "Installing hlog"
echo "============================================================"
echo "root          : ${ROOT_DIR}"
echo "prefix        : ${PREFIX}"
echo "cargo target  : ${TARGET_DIR}"
echo "cargo jobs    : ${BUILD_JOBS}"
echo

# ============================================================================
# Validate fixed public headers
# ============================================================================
if [[ ! -f "${INCLUDE_SRC}/hlog.h" ]]; then
    echo "ERROR: missing public C header:" >&2
    echo "  ${INCLUDE_SRC}/hlog.h" >&2
    exit 1
fi

if [[ ! -f "${INCLUDE_SRC}/hlog.hpp" ]]; then
    echo "ERROR: missing public C++ header:" >&2
    echo "  ${INCLUDE_SRC}/hlog.hpp" >&2
    exit 1
fi

# ============================================================================
# Build runtime
# ============================================================================
echo "[1/3] Building hlog runtime..."
env \
    -u CARGO_MAKEFLAGS \
    -u MAKEFLAGS \
    -u MFLAGS \
    CARGO_TARGET_DIR="${TARGET_DIR}" \
    cargo build \
    --manifest-path "${ROOT_DIR}/Cargo.toml" \
    -p "${RUNTIME_PACKAGE}" \
    --jobs "${BUILD_JOBS}" \
    --release

if [[ ! -f "${BUILT_SO}" ]]; then
    echo "ERROR: Cargo build succeeded but runtime was not generated:" >&2
    echo "  ${BUILT_SO}" >&2
    exit 1
fi

# ============================================================================
# Install shared library
# ============================================================================
echo "[2/3] Installing runtime..."
mkdir -p "${LIB_DST}"

cp -f \
    "${BUILT_SO}" \
    "${LIB_DST}/${SO_VERSIONED}"

ln -sfn \
    "${SO_VERSIONED}" \
    "${LIB_DST}/${SO_SONAME}"

ln -sfn \
    "${SO_SONAME}" \
    "${LIB_DST}/${SO_NAME}"

# ============================================================================
# Install headers
#
# Headers are source-controlled files. When PREFIX equals ROOT_DIR, source and
# destination are identical, so there is nothing to copy. External prefixes
# receive an explicit copy of both public headers.
# ============================================================================
echo "[3/3] Checking public headers..."

ROOT_REAL="$(realpath -m "${ROOT_DIR}")"
PREFIX_REAL="$(realpath -m "${PREFIX}")"

if [[ "${ROOT_REAL}" != "${PREFIX_REAL}" ]]; then
    mkdir -p "${INCLUDE_DST}"

    cp -f \
        "${INCLUDE_SRC}/hlog.h" \
        "${INCLUDE_DST}/hlog.h"

    cp -f \
        "${INCLUDE_SRC}/hlog.hpp" \
        "${INCLUDE_DST}/hlog.hpp"
fi

# ============================================================================
# Summary
# ============================================================================
echo
echo "============================================================"
echo "hlog installed successfully"
echo "============================================================"
echo "runtime:"
echo "  ${LIB_DST}/${SO_SONAME}"
echo
echo "headers:"
echo "  ${INCLUDE_DST}/hlog.h"
echo "  ${INCLUDE_DST}/hlog.hpp"
echo

# ============================================================================
# Optional verification
# ============================================================================
VERIFY_SCRIPT="${ROOT_DIR}/scripts/verify_runtime.sh"
if [[ -x "${VERIFY_SCRIPT}" ]]; then
    "${VERIFY_SCRIPT}" \
        "${LIB_DST}/${SO_SONAME}"
fi
