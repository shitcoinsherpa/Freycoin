# Freycoin Mining Guide (v2511.7+)

Practical, end-to-end guide for running a Freycoin mining node. Written from
operator pain points reported during v2511.6 — every section corresponds to a
real failure mode somebody hit.

## Contents

1. [Prerequisites](#prerequisites)
2. [Quick start](#quick-start)
3. [Bootstrap from a synced snapshot](#bootstrap-from-a-synced-snapshot)
4. [Building from source (portable, recommended)](#building-from-source-portable-recommended)
5. [Configuration (`freycoin.conf`)](#configuration-freycoinconf)
6. [GPU mining](#gpu-mining)
7. [Verifying the GPU is actually working](#verifying-the-gpu-is-actually-working)
8. [Multi-instance setups](#multi-instance-setups)
9. [Troubleshooting](#troubleshooting)
10. [Known good environments](#known-good-environments)

---

## Prerequisites

- Linux x86_64 (kernel 5.x or newer) or Windows 10/11 x64
- 4 GB RAM minimum, 8 GB recommended
- 30 GB free disk for chain + indexes
- For GPU mining: NVIDIA GPU, recent driver (CUDA 11.x runtime or newer; the
  CUDA Driver API is loaded dynamically — no `nvcc` needed at runtime)

The shipped binaries are statically linked. You do **not** need to install
`libevent`, `libsqlite3`, `libzmq`, `libgmp`, `libmpfr`, or `libsodium` from
your distro. If `ldd ./freycoind` shows any of those, you have the wrong build —
see [Building from source](#building-from-source-portable-recommended).

## Quick start

```bash
# 1. Pick a datadir owned by YOUR user, not root
mkdir -p ~/.freycoin

# 2. (RECOMMENDED) skip the verification grind with a bootstrap snapshot
#    — see "Bootstrap from a synced snapshot" below for why this matters.
curl -L -o /tmp/bootstrap.tar.gz \
    https://github.com/shitcoinsherpa/Freycoin/releases/download/v2511.7/freycoin-bootstrap-v2511.7-h13819.tar.gz
tar -xzf /tmp/bootstrap.tar.gz -C ~/.freycoin

# 3. Run the daemon
./freycoind -datadir=$HOME/.freycoin -daemon

# 4. Wait for sync (will print progress every block)
./freycoin-cli -datadir=$HOME/.freycoin getblockchaininfo

# 5. Get a mining address
./freycoin-cli -datadir=$HOME/.freycoin createwallet "miner"
ADDR=$(./freycoin-cli -datadir=$HOME/.freycoin getnewaddress)
echo "Mining to: $ADDR"

# 6. Start mining
./freycoin-cli -datadir=$HOME/.freycoin stop
./freycoind -datadir=$HOME/.freycoin -daemon \
    -gen=1 -mineraddress=$ADDR -genproclimit=4 -gpuintensity=80
```

That's it. The miner runs in the background; templates rebuild automatically
on every new tip arrival; valid blocks are submitted as soon as they're found
(see [Troubleshooting](#troubleshooting) if blocks are found but rejected).

## Bootstrap from a synced snapshot

**Without a bootstrap, a fresh node can spend hours to days just verifying
the chain.** Each block past the assumed-valid checkpoint (currently height
10000) requires full prime-gap PoW verification. At today's difficulty
(shift ≈ 12000) that's 30–60 seconds *per block* on a 2-vCPU box. While
verification holds `cs_main`, `getblockchaininfo` and `getblocktemplate`
will appear to time out.

A bootstrap snapshot is a tarred copy of `blocks/`, `chainstate/`, and
`indexes/` taken from a fully-synced node. After extraction the daemon
starts at near-tip and only verifies the small number of blocks produced
since the snapshot was taken.

### Use the bootstrap

```bash
# Clean datadir (or merge with existing — extracting will overwrite blocks/,
# chainstate/, indexes/ so you don't want existing wallet.dat replaced —
# but those live elsewhere)
mkdir -p ~/.freycoin

# Download the latest bootstrap (height in the filename matches its snapshot point)
curl -L -O \
    https://github.com/shitcoinsherpa/Freycoin/releases/download/v2511.7/freycoin-bootstrap-v2511.7-h13819.tar.gz
curl -L -O \
    https://github.com/shitcoinsherpa/Freycoin/releases/download/v2511.7/freycoin-bootstrap-v2511.7-h13819.tar.gz.sha256

# Verify integrity
sha256sum -c freycoin-bootstrap-v2511.7-h13819.tar.gz.sha256

# Extract into your datadir
tar -xzf freycoin-bootstrap-v2511.7-h13819.tar.gz -C ~/.freycoin

# Start the daemon — it'll do a small leveldb-recovery pass and then catch up
# the blocks since the snapshot was taken (typically minutes, not hours)
./freycoind -datadir=$HOME/.freycoin -daemon
```

### What's in the bootstrap

```
blocks/blk*.dat     # Raw block files (~70 MB at h13819)
blocks/index/       # LevelDB block index
chainstate/         # LevelDB UTXO set (~1 MB)
indexes/            # txindex + other optional indexes
```

The bootstrap does NOT contain:

- `wallet.dat` / `wallets/` — your wallets are yours, never bundled
- `peers.dat` — peer addresses are auto-discovered
- `freycoin.conf` — your config is yours
- `debug.log` — start fresh

### When to refresh the bootstrap

If the chain has advanced more than a few hundred blocks since the bootstrap
was published, downloading a fresher one is faster than catching up from the
old one. New bootstraps are published as release assets per Freycoin release
(or interim point-releases when significant time has passed).

### Building your own bootstrap

If you have a fully-synced node and want to publish/share a bootstrap, the
process is identical to what produced the official one:

```bash
# 1. Clean shutdown (CRITICAL — required for LevelDB consistency)
freycoin-cli stop
# wait for daemon to actually exit
while pgrep -f "freycoind.*$DATADIR" > /dev/null; do sleep 1; done

# 2. Tar the chain artifacts (NOT wallets, peers.dat, conf, debug.log)
HEIGHT=$(some_method_to_read_height)   # or note it from getblockcount before stop
cd $DATADIR
tar -czf /tmp/freycoin-bootstrap-v2511.7-h$HEIGHT.tar.gz blocks chainstate indexes

# 3. Generate checksum
sha256sum /tmp/freycoin-bootstrap-v2511.7-h$HEIGHT.tar.gz \
    > /tmp/freycoin-bootstrap-v2511.7-h$HEIGHT.tar.gz.sha256

# 4. Restart your daemon
freycoind -datadir=$DATADIR -daemon
```

The chain is small enough (~70 MB at h13819) that the tar takes under a
second. Daemon downtime is just the clean-stop window (typically 30–120s).

## Building from source (portable, recommended)

The pre-built binaries from `release/` are produced via the `depends/` system,
which static-links every dependency. This avoids the
"`libevent_extra-2.1.so.7: not found`" runtime error that bit miners on stock
Ubuntu 24.04+ images.

To reproduce locally:

```bash
# Linux build (host = build machine arch+OS)
./release/build-release.sh linux

# Windows cross-compile (requires mingw-w64)
./release/build-release.sh win64

# Both
./release/build-release.sh
```

The script verifies static linkage before declaring success. Output lands in
`release/linux/` and `release/win64/` along with `release/SHA256SUMS.txt`.

If you'd rather drive the underlying tools yourself:

```bash
# 1. Build dependencies (libevent etc. as STATIC libs)
gmake -C depends -j$(nproc) HOST=x86_64-pc-linux-gnu

# 2. Configure with the depends toolchain
cmake -S . -B build -G Ninja \
    --toolchain depends/x86_64-pc-linux-gnu/toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON

# 3. Build
cmake --build build -j$(nproc)

# 4. Verify
ldd build/bin/freycoind | grep -E "libevent|libsqlite|libzmq" && \
    echo "FAIL: shared deps present" || echo "OK: static-linked"
```

## Configuration (`freycoin.conf`)

A working `~/.freycoin/freycoin.conf` for a solo miner:

```ini
# Networking
listen=1
port=31470

# RPC — NOTE: use a strong password and rpcauth, not rpcuser/rpcpassword in
# production. Generated via share/rpcauth/rpcauth.py.
rpcbind=127.0.0.1
rpcport=31469
rpcallowip=127.0.0.1
rpcuser=miner
rpcpassword=CHANGE_ME

# Mining
gen=1
mineraddress=<your-address>
genproclimit=4         # CPU threads — set near your physical core count
gpuintensity=80        # 0–100; 100 = full GPU, 25 = ~25% busy with throttle

# RPC threading — defaults are now 4/16, override only if you know why.
# rpcthreads=4
# rpcworkqueue=16
```

Critical pitfalls:

- **Datadir ownership.** Never run the daemon as root and then run the CLI as
  your user — the cookie file ends up root-owned and the CLI gets "Could not
  authenticate" forever. v2511.7+ detects this at startup and refuses to run.
  Fix: `sudo chown -R "$USER:$USER" ~/.freycoin`.
- **Stale PID files.** If `freycoind` was hard-killed, `freycoind.pid` may
  point at a process that's gone. v2511.7+ detects and cleans this up.
- **`rpcthreads=16`** (the prior default) on a 2-vCPU box wakes all 16 workers
  per request via libevent's WorkQueue notification — burns CPU at idle. The
  v2511.7+ default of 4 is enough for >1000 req/s.

## GPU mining

The GPU path is enabled automatically when:

1. A NVIDIA driver is installed (`nvidia-smi` runs without error).
2. The block height has reached the GPU activation fork.
3. The candidate is large enough to benefit from GPU batch BPSW
   (currently ≥2000 bits).

Set `gpuintensity` in `freycoin.conf` (or `-gpuintensity=N` on the command
line) between 5 and 100:

- `100` — full GPU pegged at 100% (TDR-safe via 64-candidate sub-batches)
- `50`  — GPU runs ~50% of wall time, sleeps the rest
- `25`  — GPU runs ~25% of wall time (good for shared workstations)

The throttle is implemented as proportional sleep after each kernel batch,
not as fewer kernels. So `nvidia-smi` will still show short bursts to 100% —
that's expected. See next section for how to confirm the GPU is *actually*
doing work.

## Verifying the GPU is actually working

`nvidia-smi` samples utilization at ~1 Hz. Each Freycoin GPU kernel batch
takes 100–400 ms, so on a healthy node `nvidia-smi -lms 100` shows bursty
usage — but if you check at the wrong moment, you see 0% and panic.

v2511.7+ logs authoritative GPU telemetry per template. In `debug.log`, look
for lines like:

```
Mining: GPU telemetry — launches=147 candidates=9408 prp=23 gpu_ms=4180 wall_ms=12010 busy=34%
```

Field meanings:

| field        | meaning                                                          |
|--------------|------------------------------------------------------------------|
| `launches`   | number of `cuLaunchKernel` calls during this template            |
| `candidates` | total prime candidates batched on the GPU                        |
| `prp`        | candidates that GPU BPSW reported probably-prime (pre-confirm)   |
| `gpu_ms`     | cumulative GPU wall time during this template                    |
| `wall_ms`    | total time spent grinding this template                          |
| `busy`       | `gpu_ms / wall_ms` — your effective GPU duty cycle               |

Diagnostic patterns:

- `launches=0` over multiple templates → GPU path was never taken. Most
  likely: candidate size below `GPU_THRESHOLD_BITS=2000`, or
  `gpu_nextprime_init` failed at startup (check `debug.log` for
  `gpu_nextprime: GPU init failed`), or the GPU activation fork hasn't
  triggered at this height yet.
- `launches>0` but `busy=2%` → GPU is doing work but you're spending most of
  your time in CPU sieving. Increase `genproclimit` or `gpuintensity`.
- `busy ≈ gpuintensity` setpoint → working as designed.

There is also a `gpu_nextprime_debug.log` file in the working directory with
the per-segment kernel-launch detail. That file is mostly for developer
debugging — the `Mining: GPU telemetry` line in `debug.log` is the
authoritative operator signal.

## Multi-instance setups

If you're running multiple `freycoind` instances on one host (e.g. one per
GPU, or one mainnet + one regtest), each instance needs its own:

- `-datadir=` (own chainstate, peers.dat, wallets, debug.log)
- `-port=` (P2P)
- `-rpcport=` (RPC)
- `-rpcbind=127.0.0.1` (don't accidentally bind 0.0.0.0)

Example:

```bash
./freycoind -datadir=$HOME/.freycoin-1 -port=31470 -rpcport=31469 -daemon
./freycoind -datadir=$HOME/.freycoin-2 -port=31472 -rpcport=31471 -daemon
./freycoind -datadir=$HOME/.freycoin-3 -port=31474 -rpcport=31473 -daemon
```

If you forget `-rpcport=` on the second instance, v2511.7+ reports the
collision with `EADDRINUSE` and tells you exactly which PID is squatting on
the port. (Prior versions just said "Binding failed".)

## Troubleshooting

### "Binding RPC on address 127.0.0.1 port 31469 failed"

v2511.7+ replaces this bare message with one of:

- `EADDRINUSE` — another `freycoind` (or any process) holds the port. The
  message includes which PID currently has the socket.
- `EACCES` — you tried to bind a privileged port (<1024) without privilege.
- `EADDRNOTAVAIL` — the address you passed to `-rpcbind=` doesn't exist on
  this host.

If you see the original cryptic message, you're on v2511.6 or earlier — upgrade.

### "Stuck syncing past block N"

Most often this is **header sync stalled, not block sync.** The miner thread
in v2511.7+ refuses to mine until the chain has caught up to the header tip,
because mining on a stale tip just orphans your block.

Check:

```bash
freycoin-cli getblockchaininfo
# blocks: chainstate height
# headers: header tip height
# verificationprogress: 0.0–1.0
```

If `headers >> blocks`, you're sync-ing blocks. If `headers == blocks` but
`verificationprogress < 1.0`, the chainstate is still catching up. Both are
self-healing on healthy peers.

If `headers == blocks` and progress is 1.0 but the daemon insists it's
syncing, you've hit a peer-discovery problem — see the BIP155 seed format fix
in v2511.3.

### "getblockchaininfo times out / RPC busy / VERIFYING_OR_BUSY for many minutes"

This is **expected behavior on a fresh node above the assumed-valid checkpoint**
(currently height 10000). Each block past 10000 must be fully prime-gap
verified, which holds `cs_main` for 30–60 seconds per block at current
difficulty (shift ≈ 12000). While `cs_main` is held, RPC calls that require
it (including `getblockchaininfo`, `getblocktemplate`) appear to time out.

**Fix: use a bootstrap snapshot.** See [Bootstrap from a synced snapshot](#bootstrap-from-a-synced-snapshot)
above. This is the recommended path for *any* fresh node, miner or not — even
a 50-block gap can take 30+ minutes of unresponsive RPC to verify on a 2-vCPU
box.

If you've already started a fresh sync without a bootstrap, you have two
options:

1. **Wait it out.** Tail `debug.log` for `UpdateTip` lines — each one is one
   block verified. The daemon is working, just busy. RPC will become
   responsive again once the verification queue drains.
2. **Stop, replace with bootstrap, restart.** Your wallet stays intact:
   ```bash
   freycoin-cli stop
   # Wait for the daemon to actually exit
   while pgrep -f freycoind > /dev/null; do sleep 1; done
   # Wipe stale chain artifacts; KEEP wallet.dat / wallets/
   rm -rf ~/.freycoin/blocks ~/.freycoin/chainstate ~/.freycoin/indexes
   # Drop in fresh snapshot
   tar -xzf freycoin-bootstrap-v2511.7-h13819.tar.gz -C ~/.freycoin
   # Restart
   freycoind -datadir=$HOME/.freycoin -daemon
   ```

A future release will raise `assumeValidBlockHeight` to a more recent block
so this verification grind only applies to the most recent days, not weeks.

### "ProcessNewBlock: AcceptBlock FAILED (invalid-gap, prime gap proof of work failed)"

This was a v2511.6 (and earlier) bug. The miner used a slightly looser
acceptance threshold than the validator; the GPU/CPU could "find" a gap that
the authoritative `CheckProofOfWork` then rejected. v2511.7+ runs the
authoritative check pre-submission and abandons the candidate with:

```
Mining: NEAR-MISS — engine reported a valid gap but authoritative CheckProofOfWork
rejected it (height=… shift=…). This is a miner/validator threshold drift; abandoning
candidate.
```

If you see `NEAR-MISS` lines but the miner is otherwise productive, this is
the new fix doing its job — you didn't lose a block, you avoided submitting
an orphan.

### "Found block but the network ignored it"

Look for either of:

```
Mining: Found block at height=… but tip advanced during search (prev=…, tip=…) — discarding, rebuilding
Mining: Tip advanced past height=… — rebuilding template against new tip
```

These mean a competing miner won the height while you were grinding. The
v2511.7+ tip-change watchdog interrupts in-progress templates as soon as a
new tip arrives — older releases would keep grinding the dead template.

### "GPU appears idle in nvidia-smi"

Read [Verifying the GPU is actually working](#verifying-the-gpu-is-actually-working).

### "Address already in use" on second instance

You forgot `-rpcport=` and `-port=`. Both must be unique across instances on
the same host.

## Known good environments

Reported working in the wild:

- Ubuntu 22.04 LTS — daemon, Qt GUI, miner with NVIDIA driver 525+
- Ubuntu 24.04 LTS — same, with NVIDIA driver 535+
- Debian 12 — daemon and miner only (no Qt6 backport in stable)
- Windows 10/11 — pre-built `.exe` from `release/win64/`
- Hetzner CPX21 (2 vCPU / 4 GB) — runs a non-mining node fine; CPU mining
  here is too slow to find blocks but works for testing
- Latitude.sh `c3.medium.x86` — has been used to mine successfully

If you have an environment where the v2511.7+ binaries do *not* work, please
file an issue with `ldd freycoind` output and `freycoind --version`.

---

*Document maintained by the Freycoin developers. Patches welcome — every
real-world failure mode that bites a miner deserves a section here.*
