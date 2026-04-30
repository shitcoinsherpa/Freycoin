# Phase E Release Plan — v2511.7

Operator-driven incremental release answering every miner-reported issue from
the v2511.6 deployment. Strict zero-tolerance gate: no commit until the audit
gate passes against fresh-VM Linux + Windows builds.

## Why v2511.7

Real miner reports against v2511.6 enumerated nine distinct failure modes.
Each became a tracked research-then-fix pair. v2511.7 ships only after every
fix lands and the audit gate proves them.

| Bug class | Symptom | Tracked as | Status |
|-----------|---------|-----------|--------|
| Stale checkpoint table | "Stuck syncing past height 13000 → 13097 hang" | A1 | source ✅, gate pending |
| Stale mining template | "Miner finds blocks but never wins; templates never refresh on tip change" | B1 | source ✅, gate pending |
| `invalid-gap` on submit | "ProcessNewBlock: AcceptBlock FAILED (invalid-gap, prime gap proof of work failed)" | B2 | source ✅, gate pending |
| Cryptic bind error | "Binding RPC on address … failed" with no actionable detail | C1 | source ✅, gate pending |
| Datadir ownership / stale cookie | Hours debugging "Could not authenticate" because cookie was root-owned | C2 | source ✅, gate pending |
| HTTP worker thundering herd | 380% CPU at idle, 4 keepalive RPC clients waking 16 workers | D1 | source ✅, gate pending |
| Mining metadata gap in GBT | External miners can't tell prime-gap PoW from anything else | E1 | source ✅, gate pending |
| GPU "appears idle" | nvidia-smi shows 0% even though GPU is doing real work | F1 | source ✅, gate pending |
| libevent runtime dep | `libevent_extra-2.1.so.7: not found` on stock Ubuntu 24.04 | G1 | release script ✅, gate pending |

## Versioning

CalVer per project policy:

```
CMakeLists.txt: CLIENT_VERSION_REVISION 6 → 7
git tag:        v2511.7
```

The change to `CLIENT_VERSION_REVISION` is already in the working tree.

## Gate ordering (must execute in this order)

```
1. scripts/audit_gate.sh           # Linux build + smoke + Windows build
2. (manual) review scripts/audit_gate.report
3. git add <only the v2511.7 source files>
4. git commit -m "v2511.7 — miner audit fixes"
5. git tag -s v2511.7
6. ./release/build-release.sh all  # produce final binaries
7. compute final SHA256SUMS.txt
8. git push origin magnum-opus-release
9. git push --tags
10. publish binaries
```

Step 1 builds and runs every smoke check on a fresh-VM Linux env. Step 4 is
THE first commit since the user's "no commit until full test" directive on
v2511.6. Steps 6–10 are post-commit and only happen if 1 passes.

## What gets committed in the v2511.7 commit

Source files (changes already in working tree):

```
CMakeLists.txt                              # version bump
src/kernel/checkpointdata.h                 # A1: refreshed checkpoint
src/kernel/chainparams.cpp                  # A1: nMinimumChainWork updated
src/init.cpp                                # B1, B2, C2, F1: watchdog,
                                             # pre-flight, preflight check,
                                             # GPU telemetry log
src/httpserver.cpp                          # C1, D1: bind diagnostic, thread
                                             # warning
src/httpserver.h                            # D1: lower default thread/queue
src/rpc/mining.cpp                          # E1: pow_kind/min_shift/max_shift
src/pow/gpu_accel/gpu_nextprime.h           # F1: telemetry getters
src/pow/gpu_accel/gpu_nextprime_lib.cpp     # F1: atomic counters
```

New files:

```
scripts/extract_header_batches.py           # A1 reproducibility tool
scripts/audit_gate.sh                       # K1 release gate
release/build-release.sh                    # G1 portable build
release/smoke-test.sh                       # I1 release smoke test
docs/MINING.md                              # H1 user-facing mining guide
docs/PHASE_E_RELEASE_PLAN_v2511_7.md        # this file
```

## Rollback strategy

- The git tag is signed and immutable. To "undo" a release we would tag
  `v2511.7-revoked` and publish v2511.8 with the offending change reverted.
- Operators stuck on a broken v2511.7 can downgrade by re-running v2511.6,
  but the v2511.7 chainstate is forward-compatible (no consensus changes).
  The B2 pre-flight is an additive rejection (rejects MORE candidates, not
  fewer), so a v2511.6 node will not see v2511.7-mined blocks differently.
- The A1 checkpoint refresh is a node-local optimization, not consensus —
  rolling back to v2511.6 just means the older "skip header validation"
  list is used; the chain still validates fully.

## Things that do NOT change in v2511.7

To be explicit about what's *not* in scope so reviewers can audit faster:

- No consensus rule changes
- No P2P protocol changes
- No wallet schema changes
- No new RPC methods (E1 adds fields to an existing one)
- No fork activation changes
- No GPU activation height changes

## Smoke-test matrix (I1)

`release/smoke-test.sh` runs against `release/linux/` and validates:

1. Static linkage — no `libevent_*` / `libsqlite3` / `libzmq` runtime dep
2. `--version` reports v2511.7
3. Daemon starts cleanly on regtest
4. `getblockchaininfo` returns expected fields
5. `getblocktemplate` exposes `pow_kind`, `min_shift`, `max_shift`,
   `min_difficulty` (E1)
6. Second daemon on the same RPC port reports actionable EADDRINUSE (C1)
7. Daemon shuts down cleanly on `stop` RPC
8. Datadir preflight runs and tolerates a stale PID file (C2)

Failure on any of these aborts the gate — the release does not proceed.

## Build matrix (G1)

`release/build-release.sh` produces:

- `release/linux/{freycoind, freycoin-qt, freycoin-cli, freycoin-tx, freycoin-wallet}`
- `release/win64/{freycoind.exe, freycoin-qt.exe, freycoin-cli.exe, freycoin-tx.exe, freycoin-wallet.exe}`
- `release/SHA256SUMS.txt`

Both targets statically link `libevent`, `libsqlite3`, `libzmq`, `gmp`,
`mpfr`, `sodium` via the `depends/` system. The script auto-verifies static
linkage and fails if the binary still imports any of them.

## Open questions for human reviewer

1. The audit gate runs the full Linux + Windows build pipeline locally. On a
   first-time machine, depends/ takes ~30–60 minutes to bootstrap. Is the
   gate environment an opt-in step (run on dedicated CI) or do we expect each
   maintainer to run it locally before commit?
2. Windows smoke test currently runs only the static-link check; the
   functional regtest run requires Wine in the gate environment. Acceptable?
3. The v2511.6 → v2511.7 source diff is large (B1+B2+C1+C2+D1+E1+F1+A1+H1).
   Should we land this as one squashed commit ("v2511.7 — miner audit fixes")
   or separate commits per fix? One squash is simpler for revert-as-a-unit;
   separate is better for git blame archaeology. Default is squash.

## Sign-off

The maintainer must record (in `scripts/audit_gate.report` or a release notes
PR) the OS, kernel, GLIBC, and CPU of the gate machine so future operators
can reproduce.
