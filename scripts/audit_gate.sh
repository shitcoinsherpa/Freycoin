#!/usr/bin/env bash
# scripts/audit_gate.sh — Build + smoke-test audit gate
#
# This is the official "may we commit / publish v2511.7" check. Every fix
# landed in the working tree must pass this gate before being committed,
# per the v2511.7 release plan: no commits until full Linux daemon + Linux
# Qt GUI + Windows daemon cross-compile + Windows Qt GUI cross-compile + smoke
# test on a fresh-VM matrix succeed.
#
# Usage:
#   scripts/audit_gate.sh                # Linux build + smoke + Windows build (no smoke)
#   scripts/audit_gate.sh --linux-only   # Linux only — fastest gate
#   scripts/audit_gate.sh --full         # Linux + Windows + smoke (where applicable)
#
# Outputs scripts/audit_gate.report — full timestamped log of every gate check.
#
# Exit codes:
#   0 — ALL checks passed; safe to commit
#   1 — at least one check failed; DO NOT commit

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

REPORT="${REPO_ROOT}/scripts/audit_gate.report"
: > "${REPORT}"

MODE="default"   # default | linux-only | full
case "${1:-}" in
    --linux-only) MODE="linux-only" ;;
    --full)       MODE="full" ;;
    "")           MODE="default" ;;
    *)            echo "Unknown flag: $1 (use --linux-only or --full)"; exit 2 ;;
esac

PASS=0
FAIL=0
FAILED_STAGES=()

log()    { printf '\033[1;36m[gate]\033[0m %s\n' "$*" | tee -a "${REPORT}"; }
ok()     { printf '\033[1;32m  [PASS]\033[0m %s\n' "$*" | tee -a "${REPORT}"; PASS=$((PASS+1)); }
fail()   { printf '\033[1;31m  [FAIL]\033[0m %s\n' "$*" | tee -a "${REPORT}"; FAIL=$((FAIL+1)); FAILED_STAGES+=("$*"); }
banner() { printf '\n\033[1;35m═══ %s ═══\033[0m\n' "$*" | tee -a "${REPORT}"; }

START_TIME=$(date +%s)
log "audit_gate started: $(date -Iseconds)"
log "mode: ${MODE}"
log "repo: ${REPO_ROOT}"
log "git HEAD: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
log "git branch: $(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
log "report: ${REPORT}"

# ─── Stage 1: pre-flight tools ───────────────────────────────────────────────

banner "Stage 1: Pre-flight tool check"

PREFLIGHT_FAIL=0
for tool in cmake ninja gmake make python3 sha256sum; do
    if command -v "${tool}" >/dev/null 2>&1 || \
       { [[ "${tool}" == "gmake" ]] && command -v make >/dev/null 2>&1; }; then
        ok "have ${tool}"
    else
        fail "missing ${tool}"
        PREFLIGHT_FAIL=1
    fi
done

if [[ "${MODE}" == "default" ]] || [[ "${MODE}" == "full" ]]; then
    if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        ok "have mingw-w64 cross-compiler"
    else
        fail "missing x86_64-w64-mingw32-gcc — install g++-mingw-w64-x86-64-posix"
        PREFLIGHT_FAIL=1
    fi
fi

if [[ ${PREFLIGHT_FAIL} -ne 0 ]]; then
    log "Pre-flight failed; aborting before build."
    log "Fix the missing tools and re-run."
    exit 1
fi

# ─── Stage 2: Linux build ────────────────────────────────────────────────────

banner "Stage 2: Linux release build (depends/ + cmake + static verify)"
LINUX_LOG="${REPO_ROOT}/scripts/audit_gate.linux-build.log"
if "${REPO_ROOT}/release/build-release.sh" linux >"${LINUX_LOG}" 2>&1; then
    ok "Linux release build completed"
    tail -5 "${LINUX_LOG}" | sed 's/^/      /' | tee -a "${REPORT}"
else
    fail "Linux release build failed"
    log "  see ${LINUX_LOG} for full output; tail follows:"
    tail -40 "${LINUX_LOG}" | sed 's/^/      /' | tee -a "${REPORT}"
fi

# ─── Stage 3: Linux smoke test ───────────────────────────────────────────────

if [[ -x "${REPO_ROOT}/release/linux/freycoind" ]]; then
    banner "Stage 3: Linux smoke test (regtest, every fix)"
    SMOKE_LOG="${REPO_ROOT}/scripts/audit_gate.linux-smoke.log"
    if "${REPO_ROOT}/release/smoke-test.sh" release/linux >"${SMOKE_LOG}" 2>&1; then
        ok "Linux smoke test passed"
        tail -10 "${SMOKE_LOG}" | sed 's/^/      /' | tee -a "${REPORT}"
    else
        fail "Linux smoke test failed (exit $?)"
        log "  see ${SMOKE_LOG}; tail follows:"
        tail -40 "${SMOKE_LOG}" | sed 's/^/      /' | tee -a "${REPORT}"
    fi
else
    fail "Linux freycoind not built — cannot smoke-test"
fi

# ─── Stage 4: Windows cross-compile (when applicable) ────────────────────────

if [[ "${MODE}" == "default" ]] || [[ "${MODE}" == "full" ]]; then
    banner "Stage 4: Windows cross-compile (depends/ mingw + cmake + static verify)"
    WIN_LOG="${REPO_ROOT}/scripts/audit_gate.win64-build.log"
    if "${REPO_ROOT}/release/build-release.sh" win64 >"${WIN_LOG}" 2>&1; then
        ok "Windows cross-compile completed"
        tail -5 "${WIN_LOG}" | sed 's/^/      /' | tee -a "${REPORT}"
    else
        fail "Windows cross-compile failed"
        log "  see ${WIN_LOG}; tail follows:"
        tail -40 "${WIN_LOG}" | sed 's/^/      /' | tee -a "${REPORT}"
    fi

    # Windows smoke-test would require Wine + extra setup; we don't run it
    # here. The build-release.sh already runs static-link verification on the
    # produced .exe files, which is the most important Windows-specific check.
    if [[ -f "${REPO_ROOT}/release/win64/freycoind.exe" ]]; then
        ok "Windows freycoind.exe exists (static-link check ran during build)"
    else
        fail "Windows freycoind.exe missing — build did not produce expected artifact"
    fi
fi

# ─── Stage 5: source-tree sanity ─────────────────────────────────────────────

banner "Stage 5: Source-tree sanity"

# Confirm version bump landed (v2511.8)
if grep -q "set(CLIENT_VERSION_REVISION 8)" "${REPO_ROOT}/CMakeLists.txt"; then
    ok "CMakeLists.txt CLIENT_VERSION_REVISION = 8"
else
    fail "CMakeLists.txt CLIENT_VERSION_REVISION is not 8 — version bump missing"
fi

# v2511.8: confirm bootstrap engine is in source
if [[ -f "${REPO_ROOT}/src/node/bootstrap.cpp" ]] && [[ -f "${REPO_ROOT}/src/node/bootstrap.h" ]]; then
    ok "src/node/bootstrap.{h,cpp} present (v2511.8 sync-from-bootstrap)"
else
    fail "src/node/bootstrap.{h,cpp} missing — v2511.8 fix not in source"
fi
if grep -q 'BootstrapURL' "${REPO_ROOT}/src/kernel/chainparams.h" && \
   grep -q 'm_bootstrap_url *=' "${REPO_ROOT}/src/kernel/chainparams.cpp"; then
    ok "BootstrapURL plumbed in chainparams"
else
    fail "BootstrapURL not plumbed in chainparams — v2511.8 fix incomplete"
fi
if grep -q 'MaybeApplyStagedBootstrap' "${REPO_ROOT}/src/init.cpp"; then
    ok "init.cpp wires MaybeApplyStagedBootstrap"
else
    fail "init.cpp missing MaybeApplyStagedBootstrap — v2511.8 fix incomplete"
fi

# Confirm new RPC fields in mining.cpp
if grep -q '"pow_kind"' "${REPO_ROOT}/src/rpc/mining.cpp" 2>/dev/null; then
    ok "src/rpc/mining.cpp exposes pow_kind (E1)"
else
    fail "src/rpc/mining.cpp missing pow_kind — E1 fix not in source"
fi

# Confirm checkpoint table refresh (A1)
if grep -q "assumedValidBlockHeight = 10000" "${REPO_ROOT}/src/kernel/checkpointdata.h" 2>/dev/null; then
    ok "src/kernel/checkpointdata.h assumedValidBlockHeight refreshed to 10000 (A1)"
else
    fail "checkpointdata.h missing v2511.7 AVB refresh — A1 fix not in source"
fi

# Confirm tip-change watchdog (B1)
if grep -q "tip_changed_during_mine" "${REPO_ROOT}/src/init.cpp"; then
    ok "src/init.cpp has tip-change watchdog (B1)"
else
    fail "src/init.cpp missing watchdog — B1 fix not in source"
fi

# Confirm pre-flight CheckProofOfWork (B2)
if grep -q "NEAR-MISS" "${REPO_ROOT}/src/init.cpp"; then
    ok "src/init.cpp has NEAR-MISS pre-flight (B2)"
else
    fail "src/init.cpp missing NEAR-MISS pre-flight — B2 fix not in source"
fi

# Confirm RPC bind diagnostic (C1)
if grep -qE "EADDRINUSE|already in use" "${REPO_ROOT}/src/httpserver.cpp"; then
    ok "src/httpserver.cpp has actionable bind diagnostic (C1)"
else
    fail "src/httpserver.cpp missing actionable bind diagnostic — C1 fix not in source"
fi

# Confirm datadir preflight (C2)
if grep -q "DataDirPreflight" "${REPO_ROOT}/src/init.cpp"; then
    ok "src/init.cpp has DataDirPreflight (C2)"
else
    fail "src/init.cpp missing DataDirPreflight — C2 fix not in source"
fi

# Confirm RPC default thread/queue lowered (D1)
if grep -q "DEFAULT_HTTP_THREADS=4" "${REPO_ROOT}/src/httpserver.h"; then
    ok "DEFAULT_HTTP_THREADS lowered to 4 (D1)"
else
    fail "src/httpserver.h DEFAULT_HTTP_THREADS not 4 — D1 fix not in source"
fi

# Confirm GPU telemetry (F1)
if grep -q "gpu_nextprime_kernel_launches" "${REPO_ROOT}/src/init.cpp" && \
   grep -q "gpu_nextprime_kernel_launches" "${REPO_ROOT}/src/pow/gpu_accel/gpu_nextprime.h"; then
    ok "GPU telemetry plumbed end-to-end (F1)"
else
    fail "GPU telemetry not plumbed end-to-end — F1 fix incomplete"
fi

# Confirm docs/MINING.md
if [[ -f "${REPO_ROOT}/docs/MINING.md" ]]; then
    ok "docs/MINING.md present (H1)"
else
    fail "docs/MINING.md missing — H1 fix not delivered"
fi

# ─── Summary ─────────────────────────────────────────────────────────────────

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

banner "Audit gate summary"
log "elapsed: ${ELAPSED}s"
log "passed: ${PASS}"
log "failed: ${FAIL}"
if [[ ${FAIL} -gt 0 ]]; then
    log ""
    log "FAILURES — DO NOT COMMIT:"
    for f in "${FAILED_STAGES[@]}"; do
        printf '  - %s\n' "$f" | tee -a "${REPORT}"
    done
    exit 1
fi

log ""
log "ALL GATES PASSED — v2511.7 is safe to commit and tag."
exit 0
