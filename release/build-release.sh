#!/usr/bin/env bash
# release/build-release.sh — Build portable, statically-linked release binaries
# via the depends/ system. Produces Linux x86_64 + Windows x86_64 daemon and
# Qt GUI binaries with no system libevent / libsqlite3 / libzmq runtime deps.
#
# v2511.7+: replaces the prior "build against system libs" path that produced
# the libevent_extra-2.1.so.7 runtime dependency miners hit on stock VPS images.
#
# Usage:
#   release/build-release.sh                    # both Linux and Windows
#   release/build-release.sh linux              # Linux only
#   release/build-release.sh win64              # Windows only
#   JOBS=16 release/build-release.sh            # override parallelism
#   SKIP_DEPENDS=1 release/build-release.sh     # reuse pre-built depends
#
# Output:
#   release/linux/   — freycoind, freycoin-qt, freycoin-cli, freycoin-tx, freycoin-wallet
#   release/win64/   — *.exe equivalents
#   release/SHA256SUMS.txt
#
# Verification (run by this script, will fail the build if violated):
#   - Linux:   ldd OUT/freycoind | grep -E "libevent|libsqlite|libzmq" → MUST be empty
#   - Windows: objdump -p OUT/freycoind.exe | grep "DLL Name:" → MUST NOT include libevent/libsqlite/libzmq

set -euo pipefail

# ─── Paths ───────────────────────────────────────────────────────────────────

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RELEASE_DIR="${REPO_ROOT}/release"
DEPENDS_DIR="${REPO_ROOT}/depends"
BUILD_ROOT="${REPO_ROOT}/build-release"

JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
SKIP_DEPENDS="${SKIP_DEPENDS:-0}"

# ─── Targets ─────────────────────────────────────────────────────────────────

LINUX_HOST="x86_64-pc-linux-gnu"
WIN64_HOST="x86_64-w64-mingw32"

WANT_LINUX=0
WANT_WIN64=0
case "${1:-all}" in
    all)   WANT_LINUX=1; WANT_WIN64=1 ;;
    linux) WANT_LINUX=1 ;;
    win64) WANT_WIN64=1 ;;
    *)     echo "Unknown target: $1 (use: all|linux|win64)"; exit 2 ;;
esac

# ─── Logging helpers ─────────────────────────────────────────────────────────

log() { printf '\033[1;36m[release]\033[0m %s\n' "$*"; }
err() { printf '\033[1;31m[release ERROR]\033[0m %s\n' "$*" >&2; }
die() { err "$*"; exit 1; }

# ─── Pre-flight ──────────────────────────────────────────────────────────────

[[ -d "${DEPENDS_DIR}" ]] || die "depends/ not found at ${DEPENDS_DIR}"
[[ -f "${REPO_ROOT}/CMakeLists.txt" ]] || die "CMakeLists.txt not found"

command -v gmake >/dev/null 2>&1 && MAKE=gmake || MAKE=make
command -v cmake >/dev/null 2>&1 || die "cmake required"
command -v ninja >/dev/null 2>&1 || die "ninja required (apt install ninja-build)"

if [[ ${WANT_WIN64} -eq 1 ]]; then
    command -v "${WIN64_HOST}-gcc"   >/dev/null 2>&1 \
        || die "${WIN64_HOST}-gcc not found (apt install g++-mingw-w64-x86-64-posix)"
    command -v "${WIN64_HOST}-g++"   >/dev/null 2>&1 \
        || die "${WIN64_HOST}-g++ not found"
    command -v "${WIN64_HOST}-objdump" >/dev/null 2>&1 \
        || die "${WIN64_HOST}-objdump not found (binutils-mingw-w64)"
fi

# ─── Build one target ────────────────────────────────────────────────────────

build_target() {
    local host="$1"
    local label="$2"
    local outdir="${RELEASE_DIR}/${label}"
    local builddir="${BUILD_ROOT}/${label}"

    log "── Building ${label} (host=${host}) ──"

    if [[ "${SKIP_DEPENDS}" != "1" ]]; then
        log "[depends] gmake -C depends HOST=${host} -j${JOBS}"
        ${MAKE} -C "${DEPENDS_DIR}" HOST="${host}" -j"${JOBS}"
    else
        log "[depends] SKIP_DEPENDS=1, reusing existing ${DEPENDS_DIR}/${host}"
        [[ -d "${DEPENDS_DIR}/${host}" ]] \
            || die "SKIP_DEPENDS=1 set but ${DEPENDS_DIR}/${host} doesn't exist"
    fi

    local toolchain="${DEPENDS_DIR}/${host}/toolchain.cmake"
    [[ -f "${toolchain}" ]] || die "toolchain not generated: ${toolchain}"

    log "[cmake] configure ${builddir}"
    rm -rf "${builddir}"
    cmake -S "${REPO_ROOT}" -B "${builddir}" -G Ninja \
        --toolchain "${toolchain}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_GUI=ON

    log "[cmake] build (-j${JOBS})"
    cmake --build "${builddir}" -j"${JOBS}"

    log "[install] -> ${outdir}"
    rm -rf "${outdir}"
    mkdir -p "${outdir}"

    if [[ "${host}" == *mingw32* ]]; then
        for bin in freycoind freycoin-qt freycoin-cli freycoin-tx freycoin-wallet; do
            cp -v "${builddir}/bin/${bin}.exe" "${outdir}/" 2>/dev/null || true
        done
        verify_win64 "${outdir}"
    else
        for bin in freycoind freycoin-qt freycoin-cli freycoin-tx freycoin-wallet; do
            cp -v "${builddir}/bin/${bin}" "${outdir}/" 2>/dev/null || true
        done
        verify_linux "${outdir}"
    fi
}

# ─── Verification ────────────────────────────────────────────────────────────

# A statically-linked release MUST NOT depend on system libevent/libsqlite3/libzmq.
# These are exactly the libs that bit miners on stock Ubuntu 24.04+: the system
# package version differs from depends/, so the loader fails at startup.
FORBIDDEN_LIBS_REGEX='libevent|libsqlite3|libzmq|libgmp\.so|libmpfr\.so|libsodium'

verify_linux() {
    local outdir="$1"
    local fail=0
    for bin in freycoind freycoin-qt freycoin-cli freycoin-tx freycoin-wallet; do
        local p="${outdir}/${bin}"
        [[ -x "${p}" ]] || { log "  [skip] ${bin} not built"; continue; }

        local deps
        deps=$(ldd "${p}" 2>/dev/null || true)
        if echo "${deps}" | grep -E "${FORBIDDEN_LIBS_REGEX}" >/dev/null; then
            err "${bin} has forbidden runtime deps:"
            echo "${deps}" | grep -E "${FORBIDDEN_LIBS_REGEX}" >&2
            fail=1
        else
            log "  [ok] ${bin}: no forbidden runtime deps"
        fi
    done
    [[ ${fail} -eq 0 ]] || die "Linux static-link verification FAILED"
}

verify_win64() {
    local outdir="$1"
    local fail=0
    for bin in freycoind freycoin-qt freycoin-cli freycoin-tx freycoin-wallet; do
        local p="${outdir}/${bin}.exe"
        [[ -f "${p}" ]] || { log "  [skip] ${bin}.exe not built"; continue; }

        local imports
        imports=$("${WIN64_HOST}-objdump" -p "${p}" | grep -i "DLL Name:" || true)
        if echo "${imports}" | grep -iE "libevent|sqlite3|zmq|gmp|mpfr|sodium" >/dev/null; then
            err "${bin}.exe has forbidden DLL imports:"
            echo "${imports}" | grep -iE "libevent|sqlite3|zmq|gmp|mpfr|sodium" >&2
            fail=1
        else
            log "  [ok] ${bin}.exe: no forbidden DLL imports"
        fi
    done
    [[ ${fail} -eq 0 ]] || die "Windows static-link verification FAILED"
}

# ─── Execute ─────────────────────────────────────────────────────────────────
# NOTE: use `if`-form rather than `[[ ... ]] && cmd` here. Under `set -e`, the
# short-circuit form returns non-zero when the test is false, killing the script
# even though "this target wasn't requested" is the expected, healthy case.

if [[ ${WANT_LINUX} -eq 1 ]]; then build_target "${LINUX_HOST}" "linux"; fi
if [[ ${WANT_WIN64} -eq 1 ]]; then build_target "${WIN64_HOST}" "win64"; fi

# ─── Checksums ───────────────────────────────────────────────────────────────

log "[checksum] release/SHA256SUMS.txt"
cd "${RELEASE_DIR}"
{
    if [[ ${WANT_LINUX} -eq 1 ]]; then find linux -type f -exec sha256sum {} \;; fi
    if [[ ${WANT_WIN64} -eq 1 ]]; then find win64 -type f -exec sha256sum {} \;; fi
} | sort -k2 > SHA256SUMS.txt

log "Done. Outputs:"
if [[ ${WANT_LINUX} -eq 1 ]]; then ls -la "${RELEASE_DIR}/linux/"; fi
if [[ ${WANT_WIN64} -eq 1 ]]; then ls -la "${RELEASE_DIR}/win64/"; fi
log "Checksums: ${RELEASE_DIR}/SHA256SUMS.txt"
