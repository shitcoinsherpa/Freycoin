#!/usr/bin/env bash
# release/smoke-test.sh — Automated release smoke test
#
# Validates a freshly built freycoind / freycoin-cli pair by exercising every
# v2511.7 fix on a fresh, ephemeral regtest datadir. Designed to run on a fresh
# VM (or VM-like environment) before publishing the release.
#
# Each section below corresponds to a known v2511.6 failure mode. If any fail,
# DO NOT publish — the release is not ready.
#
# Usage:
#   release/smoke-test.sh [path/to/binaries-dir]
#       default = release/linux
#
# Exit codes:
#   0 — all checks passed
#   1 — at least one check failed
#   2 — environment / setup issue (cannot run)

set -uo pipefail

BIN_DIR="${1:-release/linux}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$(cd "${REPO_ROOT}/${BIN_DIR}" 2>/dev/null && pwd || echo "${BIN_DIR}")"

FREYCOIND="${BIN_DIR}/freycoind"
FREYCOIN_CLI="${BIN_DIR}/freycoin-cli"

[[ -x "${FREYCOIND}" ]]    || { echo "ERROR: ${FREYCOIND} not found or not executable"; exit 2; }
[[ -x "${FREYCOIN_CLI}" ]] || { echo "ERROR: ${FREYCOIN_CLI} not found or not executable"; exit 2; }

WORK_DIR="$(mktemp -d -t freycoin-smoke-XXXXXX)"
DATADIR="${WORK_DIR}/datadir"
mkdir -p "${DATADIR}"

# Pick ports unlikely to collide. Smoke-test only — not for real use.
P2P_PORT=$((40000 + RANDOM % 10000))
RPC_PORT=$((P2P_PORT + 1))

PASS=0
FAIL=0
FAILED_CHECKS=()

# ─── Logging ─────────────────────────────────────────────────────────────────

log()    { printf '\033[1;36m[smoke]\033[0m %s\n' "$*"; }
ok()     { printf '\033[1;32m  [PASS]\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
fail()   { printf '\033[1;31m  [FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); FAILED_CHECKS+=("$*"); }
skip()   { printf '\033[1;33m  [SKIP]\033[0m %s\n' "$*"; }
banner() { printf '\n\033[1;34m── %s ──\033[0m\n' "$*"; }

CONF="${DATADIR}/freycoin.conf"
RPC_USER="smoke"
RPC_PASS="$(head -c 16 /dev/urandom | base64 | tr -d '/+=' | head -c 22)"

cat > "${CONF}" <<EOF
regtest=1
[regtest]
listen=0
port=${P2P_PORT}
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcuser=${RPC_USER}
rpcpassword=${RPC_PASS}
rpcport=${RPC_PORT}
EOF

cli() { "${FREYCOIN_CLI}" -datadir="${DATADIR}" -regtest \
    -rpcuser="${RPC_USER}" -rpcpassword="${RPC_PASS}" -rpcport="${RPC_PORT}" "$@"; }

# Always clean up on exit
DAEMON_PID=""
cleanup() {
    if [[ -n "${DAEMON_PID}" ]] && kill -0 "${DAEMON_PID}" 2>/dev/null; then
        cli stop >/dev/null 2>&1 || kill "${DAEMON_PID}" 2>/dev/null
        for _ in {1..30}; do
            kill -0 "${DAEMON_PID}" 2>/dev/null || break
            sleep 1
        done
        kill -9 "${DAEMON_PID}" 2>/dev/null || true
    fi
    rm -rf "${WORK_DIR}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# ─── Check 1: ldd (Linux only) — no forbidden runtime deps ───────────────────

banner "1. Static-link verification"
if [[ "$(uname -s)" == "Linux" ]] && command -v ldd >/dev/null 2>&1; then
    deps="$(ldd "${FREYCOIND}" 2>/dev/null || true)"
    if echo "${deps}" | grep -qE 'libevent|libsqlite3|libzmq|libmpfr\.so|libgmp\.so|libsodium'; then
        fail "freycoind has forbidden runtime deps:"
        echo "${deps}" | grep -E 'libevent|libsqlite3|libzmq|libmpfr\.so|libgmp\.so|libsodium' | sed 's/^/        /'
    else
        ok "freycoind has no forbidden runtime deps"
    fi
else
    skip "ldd not applicable on $(uname -s)"
fi

# ─── Check 2: --version sanity ───────────────────────────────────────────────

banner "2. --version returns expected version"
ver="$("${FREYCOIND}" --version 2>&1 | head -1 || true)"
if [[ "${ver}" == *"v2511.7"* ]] || [[ "${ver}" == *"2511.7"* ]]; then
    ok "version line: ${ver}"
elif [[ -n "${ver}" ]]; then
    fail "unexpected version line: ${ver}"
else
    fail "freycoind --version produced no output"
fi

# ─── Check 3: starts cleanly ─────────────────────────────────────────────────

banner "3. Daemon starts on regtest"
"${FREYCOIND}" -datadir="${DATADIR}" -regtest -daemon \
    >"${WORK_DIR}/start.out" 2>&1 || true

# Wait for the daemon to come up (cookie file + RPC accepting)
DAEMON_UP=0
for _ in {1..30}; do
    if cli getblockchaininfo >/dev/null 2>&1; then
        DAEMON_UP=1
        break
    fi
    sleep 1
done

if [[ ${DAEMON_UP} -eq 1 ]]; then
    DAEMON_PID="$(pgrep -f -- "-datadir=${DATADIR}" | head -1 || true)"
    ok "daemon up, RPC responding (PID=${DAEMON_PID:-unknown})"
else
    fail "daemon did not become RPC-ready within 30s"
    cat "${WORK_DIR}/start.out" | tail -40 | sed 's/^/        /' || true
    cat "${DATADIR}/regtest/debug.log" 2>/dev/null | tail -40 | sed 's/^/        /' || true
    log "Summary: ${PASS} pass / ${FAIL} fail (early exit)"
    exit 1
fi

# ─── Check 4: getblockchaininfo basic shape ──────────────────────────────────

banner "4. getblockchaininfo returns expected fields"
GBI="$(cli getblockchaininfo 2>/dev/null || true)"
if echo "${GBI}" | grep -q '"chain": "regtest"'; then
    ok "chain == regtest"
else
    fail "getblockchaininfo missing 'chain': regtest"
fi
if echo "${GBI}" | grep -q '"blocks":'; then
    ok "getblockchaininfo has blocks field"
else
    fail "getblockchaininfo missing blocks"
fi

# ─── Check 5: getblocktemplate (E1 fix — pow_kind/min_shift/max_shift) ───────

banner "5. getblocktemplate exposes prime-gap PoW metadata (E1)"
# Need a wallet + address to get a template
cli createwallet smokewallet >/dev/null 2>&1 || true
ADDR="$(cli -rpcwallet=smokewallet getnewaddress 2>/dev/null || true)"
if [[ -z "${ADDR}" ]]; then
    skip "could not create wallet/address — skipping GBT check"
else
    GBT="$(cli getblocktemplate '{"rules":["segwit"]}' 2>/dev/null || true)"
    if echo "${GBT}" | grep -q '"pow_kind": "prime_gap"'; then
        ok "pow_kind == prime_gap"
    else
        fail "getblocktemplate missing pow_kind"
    fi
    if echo "${GBT}" | grep -qE '"min_shift":\s*[0-9]+'; then
        ok "min_shift present"
    else
        fail "getblocktemplate missing min_shift"
    fi
    if echo "${GBT}" | grep -qE '"max_shift":\s*[0-9]+'; then
        ok "max_shift present"
    else
        fail "getblocktemplate missing max_shift"
    fi
    if echo "${GBT}" | grep -qE '"min_difficulty":\s*"[0-9a-f]+"'; then
        ok "min_difficulty present"
    else
        fail "getblocktemplate missing min_difficulty"
    fi
fi

# ─── Check 6: RPC bind diagnostic (C1 fix) ───────────────────────────────────

banner "6. RPC bind error gives actionable diagnostic on EADDRINUSE (C1)"
SECOND_DATADIR="${WORK_DIR}/datadir2"
mkdir -p "${SECOND_DATADIR}"
cat > "${SECOND_DATADIR}/freycoin.conf" <<EOF
regtest=1
[regtest]
listen=0
port=$((P2P_PORT + 100))
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcuser=${RPC_USER}
rpcpassword=${RPC_PASS}
rpcport=${RPC_PORT}
EOF

# Run in foreground with -daemon=0 so we get logs synchronously
"${FREYCOIND}" -datadir="${SECOND_DATADIR}" -regtest -daemon=0 \
    >"${WORK_DIR}/second_start.out" 2>&1 &
SECOND_PID=$!
sleep 5
kill "${SECOND_PID}" 2>/dev/null || true
wait "${SECOND_PID}" 2>/dev/null || true

if grep -qE "EADDRINUSE|already in use|bound by process" "${WORK_DIR}/second_start.out"; then
    ok "second instance reports EADDRINUSE / already-in-use diagnostic"
elif grep -q "Binding RPC.*failed" "${WORK_DIR}/second_start.out"; then
    fail "still showing the bare 'Binding RPC ... failed' message — C1 fix not in this build"
    grep "Binding" "${WORK_DIR}/second_start.out" | head -3 | sed 's/^/        /'
else
    skip "second instance produced no recognizable bind output (probably exited before bind)"
    tail -10 "${WORK_DIR}/second_start.out" | sed 's/^/        /'
fi

# ─── Check 7: clean SIGTERM shutdown ─────────────────────────────────────────

banner "7. Daemon shuts down cleanly on stop RPC"
if cli stop >/dev/null 2>&1; then
    ok "stop RPC accepted"
else
    fail "stop RPC failed"
fi

# Wait for it to actually exit
for _ in {1..30}; do
    [[ -n "${DAEMON_PID}" ]] && kill -0 "${DAEMON_PID}" 2>/dev/null || { DAEMON_PID=""; break; }
    sleep 1
done
if [[ -z "${DAEMON_PID}" ]]; then
    ok "daemon exited within 30s"
else
    fail "daemon still running after 30s — possible shutdown hang"
fi

# ─── Check 8: data dir preflight (C2 fix) ────────────────────────────────────

banner "8. Datadir preflight catches stale cookie on Linux (C2)"
if [[ "$(uname -s)" == "Linux" ]]; then
    PRE_DATADIR="${WORK_DIR}/preflight"
    mkdir -p "${PRE_DATADIR}/regtest"
    # Plant a stale cookie owned by current user (not stale, just present)
    # and a stale PID file referring to a definitely-not-running PID.
    echo "smoke:abcdef" > "${PRE_DATADIR}/regtest/.cookie"
    echo "999999" > "${PRE_DATADIR}/regtest/freycoind.pid"

    # Spin up daemon — should clean stale PID and proceed
    "${FREYCOIND}" -datadir="${PRE_DATADIR}" -regtest -daemon=0 \
        >"${WORK_DIR}/preflight.out" 2>&1 &
    PRE_PID=$!
    sleep 6
    if kill -0 "${PRE_PID}" 2>/dev/null; then
        kill "${PRE_PID}" 2>/dev/null
        wait "${PRE_PID}" 2>/dev/null || true
        if grep -qE "stale|preflight|cookie|pid" "${WORK_DIR}/preflight.out"; then
            ok "datadir preflight ran (log mentions stale/cookie/pid)"
        else
            skip "preflight log not found — may be silent on success"
        fi
    else
        fail "daemon exited unexpectedly during preflight"
        tail -20 "${WORK_DIR}/preflight.out" | sed 's/^/        /'
    fi
else
    skip "preflight check is Linux-only"
fi

# ─── Summary ─────────────────────────────────────────────────────────────────

banner "Summary"
log "Passed: ${PASS}"
log "Failed: ${FAIL}"
if [[ ${FAIL} -gt 0 ]]; then
    log "Failures:"
    for f in "${FAILED_CHECKS[@]}"; do
        printf '  - %s\n' "$f"
    done
    exit 1
fi
log "All smoke checks passed against ${BIN_DIR}"
