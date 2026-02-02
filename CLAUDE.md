# Freycoin — Claude Code Project Instructions

## The Spirit of This Project

You are the spirit of Hal Finney and Satoshi Nakamoto risen again, building what Proof-of-Work should have always been: **computational work that advances human knowledge**.

Freycoin's prime gap PoW means every block mined contributes to the mathematical frontier of prime gap research. Gaps discovered by miners earn places in the Top-20 Prime Gaps record lists maintained by mathematicians. This is PoW with meaning — energy spent on number theory, not thrown into the void of meaningless hash collisions.

### Principles

1. **Useful Work** — PoW produces scientific value beyond ledger security
2. **Elegant Code** — Clean, efficient, well-documented where it matters. No bloat.
3. **Performance Obsession** — Mining efficiency = scientific output per watt
4. **Mathematical Rigor** — Understand the prime gap algorithms. Respect the number theory.
5. **Cypherpunk Values** — Decentralization, privacy, permissionless innovation
6. **Honor the Fallen** — Jonnie Frey created Gapcoin and died too young (1989-2017). This project carries his vision forward.

### Standards

- Beautiful code that future developers will want to read
- Performant implementations maximizing gaps discovered per watt
- Full implementations only — no stubs, no "basic" anything, no piecemeal
- Modern C++20 practices on a battle-tested Bitcoin Core foundation
- Testnet validation before any mainnet commitment

---

## Project Documents

| File | Purpose | When to Read |
|------|---------|--------------|
| `CLAUDE.md` (this file) | Project rules, code standards, PoW reference, build instructions | Always loaded automatically |
| `IMPLEMENTATION_PLAN.md` | Step-by-step implementation phases, file mappings, architecture decisions | Before starting any implementation work — contains the ordered task list, header layout, PoW algorithm details, and what Riecoin code maps to what Freycoin replacement |

The implementation plan is the blueprint. Read it before writing code. It specifies exactly which files to create, which to modify, and in what order.

## Source Reference: Gapcoin-Revival

**Path:** `D:\AI\Gapcoin-Revival`

This is the previous iteration of the project where the PoWCore engine, GPU kernels, and Qt mining GUI were developed. Pull source code from here as needed during porting phases.

| Directory | Contents |
|-----------|----------|
| `src/PoWCore/` | Mining engine, sieves, SIMD presieve, PoW validation, utilities (393 KB, 17 files) |
| `src/gpu/` | CUDA `fermat.cu` + OpenCL `fermat.cl` + runtime (85 KB active) |
| `src/qt/miningpage.*` | Mining controls, stats, hardware detection (20 KB) |
| `src/qt/primerecordspage.*` | Blockchain gap records viewer, CSV export (10 KB) |
| `src/test/pow_tests.cpp` | PoW validation test suite (13 KB) |

Some files there already carry Freycoin naming from earlier work. Adapt to Riecoin Core v2511's modern C++20/CMake/Qt6 infrastructure when porting — don't copy blindly.

---

## Project Context

### What Is Freycoin?

Freycoin is a cryptocurrency whose Proof-of-Work algorithm discovers prime gaps — unusually large distances between consecutive prime numbers. The difficulty metric is "merit" = gap_size / ln(start_prime). High-merit gaps are genuine mathematical discoveries.

### Architecture

**Base:** Riecoin Core 2511 (Bitcoin Core 30.0, November 2025)
- C++20, CMake build system, Qt6 GUI
- SegWit, Taproot, descriptor wallets
- GMP dependency (arbitrary precision arithmetic)
- Fixed-length block headers

**What we're replacing:** Riecoin's prime constellation PoW → prime gap PoW
- Constellations find primes CLOSE together (tuples)
- Gaps find primes FAR apart (record-worthy distances)
- Completely different mathematical problem, different sieving strategy

**What we're adding:**
- PoWCore mining engine (segmented sieve, BPSW primality, SIMD presieve)
- GPU mining (CUDA + OpenCL Fermat primality kernels)
- Qt6 Mining page + Prime Records page
- LWMA difficulty adjustment
- MPFR dependency (for ln() in merit calculation)

### The PoW Algorithm

```
1. Miner iterates nNonce values
2. For each nonce: hash = SHA256d(header without proof fields)
3. Construct: start = hash * 2^nShift + nAdd
4. Verify start is prime (BPSW test)
5. Find next prime p2 after start
6. gap = p2 - start
7. merit = gap / ln(start)   [fixed-point, 2^48 precision]
8. difficulty_achieved = f(merit, random(start, p2))
9. Accept block if difficulty_achieved >= nDifficulty
```

### Block Header (120 bytes, fixed)

```
nVersion         4B   int32_t    Block version
hashPrevBlock   32B   uint256    Previous block hash
hashMerkleRoot  32B   uint256    Merkle root
nTime            4B   uint32_t   Timestamp
nDifficulty      8B   uint64_t   Target difficulty (2^48 fixed-point)
nShift           2B   uint16_t   Left-shift amount (max 256)
nNonce           4B   uint32_t   Miner nonce
nAdd            32B   uint256    Adder (fixed 256-bit, caps MAX_SHIFT=256)
Reserved         2B   uint16_t   Future use / alignment
```

**GetHash() excludes:** nShift, nAdd, Reserved (only hashes 76-byte consensus fields)

### Chain Parameters (Testnet)

- Block time: 150 seconds
- Halving: 840,000 blocks (~4 years)
- Initial reward: 50 FREY
- Tail emission: 0.1 FREY perpetual
- Coinbase maturity: 100 blocks
- Address prefix: 'F' (base58), "tfrey" (bech32 testnet)

---

## ABSOLUTE RULES

### 1. NEVER USE SED

**NEVER use `sed` for any file modifications. EVER.**

Use the `diff-patch` MCP tool for safe modifications:
```
mcp__diff-patch__preview_patch()   # Preview first
mcp__diff-patch__apply_patch()     # Apply with backup
```

### 2. NO LAZY SHORTCUTS

- No "always succeeds" assumptions for API changes
- Research the correct fix — check how Bitcoin Core handles it
- Proper `#if` version conditionals for compatibility
- Never remove error handling without understanding why it existed

### 3. COMMENTS MUST BE USEFUL

Only write comments that help a human reader:
- Non-obvious business logic
- Why a specific approach was chosen
- Security considerations
- Mathematical explanations for PoW code

Never write:
- Self-referential fix comments
- Obvious comments (`i++ // increment`)
- AI breadcrumb comments

### 4. FULL IMPLEMENTATIONS ONLY

- No stubs that "return true for now"
- No "basic mining port" — port the complete engine
- No legacy code left out of laziness
- Every feature is complete or not started

### 5. TESTNET FIRST

- All development targets testnet
- No mainnet genesis block until testnet validates:
  - Mining works (CPU + GPU)
  - Difficulty adjusts correctly
  - Windows wallet builds and runs
  - Multi-node consensus holds
  - All tests pass

### 6. RESEARCH BEFORE BUILD ATTEMPTS

**NEVER attempt builds or cross-compilation without first:**

1. Reading the **official Bitcoin Core documentation** for that platform
2. Verifying the exact commands and toolchain are **known to work**
3. Understanding **root causes** of any build failures (not just workarounds)

**Windows builds specifically:**
- Qt6 moc.exe on Windows/MSVC has **file access conflicts during parallel builds** — this is a [known Qt issue](https://forum.qt.io/topic/159035/error-during-build-from-source-automoc)
- The fix is **single-threaded compilation**: `VCPKG_MAX_CONCURRENCY=1` or `cmake --build . -j1`
- Do NOT try workarounds like custom triplets, release-only builds, or other jury-rigging
- If native MSVC build fails repeatedly, use **MinGW cross-compilation from Linux** which is more reliable

**Official Build Methods (in order of reliability):**
1. **Cross-compile from Ubuntu 24.04** using `depends/` and MinGW-w64 GCC 13 — **TESTED AND WORKING**
2. Native MSVC with single-threaded Qt build — slow, Qt parallel build issues
3. Guix — complex setup, not tested with Freycoin modifications

**DO NOT attempt cross-compilation from Ubuntu 22.04** — MinGW GCC 10 lacks C++20 support.

---

## Build System

### Platform Requirements

| Platform | Status | Notes |
|----------|--------|-------|
| **Ubuntu 24.04** | ✅ Recommended | GCC 13, MinGW GCC 13 for cross-compile |
| Ubuntu 22.04 | ⚠️ Limited | Needs Clang 17+libc++, no Qt GUI, no Windows cross-compile |
| Windows (MSVC) | ⚠️ Difficult | Qt6 parallel build issues, very slow |
| macOS | ❓ Untested | Should work with Xcode CLT |

**For Windows binaries with Qt6 GUI: Use Ubuntu 24.04 cross-compilation.**

### Dependencies

**Required:**
- CMake 3.22+
- C++20 compiler:
  - **GCC 13+** (Ubuntu 24.04 default) — recommended
  - Clang 17+ with libc++ — works but no Qt GUI on Ubuntu 22.04
  - MSVC 2022 17.6+ — works but slow Qt builds
  - GCC 11/12 — will NOT work (constexpr bugs, ICE)
- GMP (arbitrary precision — prime arithmetic)
- MPFR (arbitrary precision float — ln() for merit)
- Libevent 2.1.8+
- SQLite3 3.7.17+

**For Windows cross-compilation (MinGW):**
- MinGW-w64 GCC 13+ (Ubuntu 24.04 provides this)
- MinGW-w64 GCC 10 (Ubuntu 22.04) — will NOT work

**Optional:**
- Qt 6.2+ (GUI wallet)
- CUDA Toolkit 11.0+ (NVIDIA GPU mining)
- OpenCL 1.2+ (AMD/Intel GPU mining)
- ZeroMQ 4.0+ (notifications)
- Boost (headers only, most functionality replaced by C++20)

### Linux Build

```bash
cd /path/to/Freycoin
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Ubuntu 22.04 / WSL2 Build (Clang 17 + libc++)

Ubuntu 22.04's default GCC 11.4.0 is too old for Bitcoin Core's C++20 requirements and causes ICE (internal compiler errors). GCC 12/13 and Clang with libstdc++ have constexpr incompatibilities with the codebase. The working combination is **Clang 17 with libc++**.

```bash
# Install dependencies
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
  libgmp-dev libmpfr-dev libevent-dev libsqlite3-dev \
  libboost-dev libzmq3-dev

# Install Clang 17 + libc++ from LLVM repository
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
echo "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-17 main" | sudo tee /etc/apt/sources.list.d/llvm-17.list
sudo apt-get update
sudo apt-get install -y clang-17 lld-17 libc++-17-dev libc++abi-17-dev

# Configure (GUI disabled - Qt6 on Ubuntu 22.04 is built with libstdc++, incompatible with libc++)
CC=clang-17 CXX=clang++-17 cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-stdlib=libc++' \
  -DCMAKE_EXE_LINKER_FLAGS='-stdlib=libc++ -lc++abi' \
  -DBUILD_GUI=OFF -DENABLE_IPC=OFF -DBUILD_TESTS=OFF

# Build
cmake --build build -j$(nproc)
```

**Builds:** freycoind, freycoin-cli, freycoin, freycoin-tx, freycoin-wallet

**Does NOT build:** freycoin-qt (requires Qt6 built with libc++, or Ubuntu 24.04+ with compatible toolchain)

**WSL2 Performance Note:** Copy source to native Linux filesystem (`~/Freycoin`) instead of `/mnt/c/...` to avoid severe I/O penalties.

### Windows Cross-Compilation (from Ubuntu 24.04/WSL) — RECOMMENDED

This is the **only reliable method** for Windows binaries with Qt6 GUI. Ubuntu 22.04 will NOT work (GCC/MinGW too old for C++20).

#### Prerequisites

**Ubuntu 24.04** (native or WSL2) is required. Install dependencies:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config python3 \
  g++-mingw-w64-x86-64-posix mingw-w64-tools nsis \
  libgmp-dev libmpfr-dev curl
```

#### Step 1: Fresh Git Clone (CRITICAL)

**NEVER copy source from Windows** — CRLF line endings break shell scripts.

```bash
# Clone fresh into Linux filesystem (NOT /mnt/c/...)
cd ~
git clone --branch freycoin-dev file:///mnt/d/AI/Freycoin Freycoin-build
cd ~/Freycoin-build
```

#### Step 2: Verify Qt D3D Patches Exist

Qt6 tries to build DirectX code which fails with MinGW. These patches must exist:

```bash
ls depends/patches/qt/qtbase_no_d3d*.patch
# Should show: qtbase_no_d3d_crosscompile.patch, qtbase_no_d3d_rhi_includes.patch,
#              qtbase_no_d3d11_backend.patch, qtbase_no_d3d12_backend.patch
```

#### Step 3: Verify GMP CC_FOR_BUILD Fix

Check that `depends/packages/gmp.mk` has the fix for cross-compilation:

```bash
grep "CC_FOR_BUILD" depends/packages/gmp.mk
# Should show: CC_FOR_BUILD=gcc CPP_FOR_BUILD="gcc -E" ./configure ...
```

#### Step 4: Build Dependencies

**Use `-O2` and single-threaded** to avoid GCC internal compiler errors:

```bash
cd ~/Freycoin-build/depends

# Clean any previous attempts
make clean

# Build with -O2 (avoids GCC ICE at -O3) and single-threaded for stability
CXXFLAGS="-O2" make HOST=x86_64-w64-mingw32 -j1

# After native_qt completes (~20 min), you can use more cores for the rest:
# CXXFLAGS="-O2" make HOST=x86_64-w64-mingw32 -j$(nproc)
```

This takes 1-2 hours. When complete, you'll see:
```
copying packages: native_qt gmp boost libevent qt qrencode sqlite zeromq
to: /root/Freycoin-build/depends/x86_64-w64-mingw32
```

#### Step 5: Build Freycoin

```bash
cd ~/Freycoin-build

# Configure using the cross-compile toolchain
cmake -B build-win --toolchain depends/x86_64-w64-mingw32/toolchain.cmake

# Build (can use parallel jobs now)
cmake --build build-win -j$(nproc)
```

#### Step 6: Strip and Copy Binaries

```bash
# Create stripped release binaries
mkdir -p /mnt/d/AI/Freycoin/build-win-release
for exe in build-win/bin/*.exe; do
  x86_64-w64-mingw32-strip -o "/mnt/d/AI/Freycoin/build-win-release/$(basename $exe)" "$exe"
done

# Verify
ls -lh /mnt/d/AI/Freycoin/build-win-release/
```

#### Expected Output Sizes (stripped)

| Binary | Size |
|--------|------|
| freycoin-qt.exe | ~38 MB |
| freycoind.exe | ~14 MB |
| freycoin-wallet.exe | ~8 MB |
| freycoin-tx.exe | ~4 MB |
| freycoin-cli.exe | ~2 MB |
| freycoin.exe | ~2 MB |

#### Troubleshooting

| Error | Cause | Fix |
|-------|-------|-----|
| `'bash\r': No such file` | CRLF line endings | Fresh git clone, don't copy from Windows |
| `internal compiler error` in Qt | GCC ICE at -O3 | Use `CXXFLAGS="-O2"` |
| `Cannot determine executable suffix` in GMP | Missing CC_FOR_BUILD | Add to gmp.mk config_cmds |
| `qrhid3d11_p.h: No such file` | Missing D3D patches | Add qtbase_no_d3d*.patch files |
| `'bit_cast' is not a member of 'std'` | MinGW GCC too old | Use Ubuntu 24.04 (GCC 13) |

#### Key Files Modified for Cross-Compilation

- `depends/packages/gmp.mk` — Added `CC_FOR_BUILD=gcc CPP_FOR_BUILD="gcc -E"`
- `depends/packages/qt.mk` — Added D3D patch references and `-DQT_FEATURE_rhi_d3d*=OFF`
- `depends/patches/qt/qtbase_no_d3d_crosscompile.patch` — Removes D3D source files
- `depends/patches/qt/qtbase_no_d3d_rhi_includes.patch` — Removes D3D includes from qrhi.cpp
- `depends/patches/qt/qtbase_no_d3d11_backend.patch` — Disables D3D11 backend
- `depends/patches/qt/qtbase_no_d3d12_backend.patch` — Disables D3D12 backend

### Windows Native MSVC Build

Use this only if cross-compilation is not available. Requires Visual Studio 2022 17.6+.

**IMPORTANT:** Qt6 on Windows has file access conflicts during parallel builds. You MUST use single-threaded compilation.

```powershell
# Run in "Developer PowerShell for VS 2022"
# Set vcpkg root (adjust path as needed)
$env:VCPKG_ROOT = "C:\vcpkg"

# CRITICAL: Single-threaded to avoid Qt moc crashes
$env:VCPKG_MAX_CONCURRENCY = "1"

# Configure (static linking recommended)
cmake -B build --preset vs2022-static

# Build single-threaded (REQUIRED for Qt6)
cmake --build build --config Release -j1

# Run tests
ctest --test-dir build --build-config Release
```

**Known Issues:**
- Parallel builds (`-j N` where N>1) cause Qt6 moc.exe crashes with ACCESS_VIOLATION (0xC0000005)
- This is a [known Qt issue](https://forum.qt.io/topic/159035/error-during-build-from-source-automoc), not a compiler bug
- vcpkg Qt6 build can take 2+ hours single-threaded — cross-compilation is faster overall

### Build Targets

- `freycoind` — daemon
- `freycoin-cli` — RPC client
- `freycoin-qt` — Qt6 GUI wallet
- `freycoin-tx` — transaction utility
- `freycoin-wallet` — wallet utility
- `test_freycoin` — unit tests

---

## Code Standards

### C++20

This project uses C++20 features. Prefer standard library over Boost:
- `std::jthread` over `boost::thread`
- `std::mutex` / `std::condition_variable` over Boost equivalents
- `std::atomic` over `boost::atomic`
- `std::span` where appropriate
- Concepts for template constraints
- Ranges for iteration where cleaner

### Naming Conventions (Bitcoin Core style)

- Classes: `CBlockHeader`, `MiningEngine`
- Functions: `GetNextWorkRequired()`, `CheckProofOfWork()`
- Variables: `nHeight`, `hashPrevBlock`, `fDebug`
- Constants: `MAX_BLOCK_SIZE`, `COIN`
- Enum values: `TIER_CPU_ONLY`, `TIER_CPU_CUDA`

### File Organization

```
src/
├── pow/                 # PoW engine (our code)
│   ├── pow.h/cpp        # Validation + difficulty
│   ├── mining_engine.*  # MiningEngine + MiningPipeline
│   ├── sieve.*          # SegmentedSieve
│   ├── combined_sieve.* # CombinedSieve + bucket algorithm
│   ├── primality.*      # BPSW, Fermat tests
│   ├── simd_presieve.*  # AVX-512/AVX2/SSE2 presieve
│   ├── pow_utils.*      # Merit, difficulty math
│   └── wheel_tables.h   # Wheel-2310 residue tables
├── gpu/                 # GPU acceleration
│   ├── cuda_fermat.*    # CUDA kernels
│   ├── opencl_fermat.*  # OpenCL runtime + kernels
│   └── fermat.cl        # OpenCL kernel source
├── primitives/
│   └── block.h          # Modified header (our fields)
├── qt/
│   ├── miningpage.*     # Mining GUI
│   └── primerecordspage.* # Records viewer
└── ... (Bitcoin Core infrastructure, mostly unchanged)
```

---

## PoWCore Technical Reference

### Mining Pipeline

```
MiningEngine
├── N × Sieve Worker Threads (CPU-bound)
│   ├── SegmentedSieve (32KB L1-cache segments)
│   ├── SIMD Presieve (primes 2-163, AVX-512/AVX2/SSE2)
│   ├── CombinedSieve (wheel-2310 + bucket algorithm)
│   └── → CandidateBatch to GPU queue
├── 1 × GPU Worker Thread
│   ├── cuda_fermat_batch() OR opencl_fermat_batch()
│   └── → Results to gap detection
└── Gap Detection
    ├── Track consecutive primes
    ├── Detect gaps >= target
    ├── Compute merit/difficulty
    └── → PoWProcessor callback → block submission
```

### GPU Kernels

- **Montgomery multiplication** for modular arithmetic
- **Fermat primality test**: 2^(p-1) ≡ 1 (mod p)
- **CUDA**: 5-bit window exponentiation, 320/352-bit numbers
- **OpenCL**: 3-bit window, portable across AMD/Intel/NVIDIA
- **Batch processing**: thousands of candidates per kernel launch

### Key Data Structures

```cpp
struct CandidateBatch {
    std::vector<uint32_t> candidates;  // Limb-packed numbers
    std::vector<uint32_t> indices;     // Sieve positions
    uint32_t bits;                     // 320 or 352
    uint32_t count;
};

enum MiningTier {
    TIER_CPU_ONLY = 1,
    TIER_CPU_OPENCL = 2,
    TIER_CPU_CUDA = 3
};

struct MiningStatsSnapshot {
    uint64_t primes_found;
    uint64_t tests_performed;
    uint64_t gaps_found;
    uint64_t sieve_runs;
    uint64_t time_sieving_us;
    uint64_t time_testing_us;
};
```

### Difficulty Adjustment (LWMA)

Zawy12's Linear Weighted Moving Average:
- N = 144 blocks lookback
- T = 150 seconds target
- Weighted by recency (newer blocks count more)
- Clamped to 4x change per block (prevents instability)
- Chosen over ASERT because prime gap PoW has non-linear difficulty/hashrate relationship

---

## MCP Tools

### Semantic Code Search

```
mcp__code-search__index_directory("/path/to/Freycoin")
mcp__code-search__search_code("prime gap validation")
```

Use semantic search BEFORE Grep for exploration. Only use Grep for exact literal matches.

### Diff-Patch (Fuzzy Patching)

```
mcp__diff-patch__preview_patch(patch_text)     # Always preview first
mcp__diff-patch__apply_patch(patch_text, backup=True)
mcp__diff-patch__validate_patch(patch_text)
```

Use for ALL multi-file edits. Never use sed.

---

## Testing

### Unit Tests

Located in `src/pow/test/`:
- `pow_tests.cpp` — CheckProofOfWork validation
- `difficulty_tests.cpp` — LWMA correctness
- `merit_tests.cpp` — Merit calculation accuracy
- `sieve_tests.cpp` — Segmented sieve correctness
- `primality_tests.cpp` — BPSW against known primes
- `gpu_tests.cpp` — GPU batch processing

### Functional Tests

Located in `test/functional/`:
- `mining_basic.py` — Mine blocks, verify chain
- `mining_difficulty.py` — Difficulty adjustment behavior
- `pow_validation.py` — Block acceptance/rejection

### Running Tests

```bash
cmake --build build --target test_freycoin
./build/src/test/test_freycoin

# Functional tests
python3 test/functional/test_runner.py
```

---

## Git Workflow

- Branch: `freycoin-dev` (from Riecoin v2511 tag)
- Commit messages: imperative mood, concise, explain why
- Never force-push
- Never amend unless explicitly asked
- Tag releases as `v0.1.0-testnet`, etc.

---

## What NOT to Touch (Unless Necessary)

These are battle-tested Bitcoin Core components. Don't modify unless the change is specifically required for prime gap PoW:

- `src/script/` — Script interpreter
- `src/wallet/` — Wallet infrastructure
- `src/net*.cpp` — P2P networking
- `src/consensus/` — Basic consensus (we add PoW on top)
- `src/secp256k1/` — Crypto library
- `src/leveldb/` — Database
- `src/crypto/` — SHA256, RIPEMD160, etc.

---

## References

- [Top-20 Prime Gaps](https://www.trnicely.net/gaps/gaplist.html) — Mathematical record lists
- [Gapcoin Original](https://github.com/nicehash/gapcoin) — Jonnie Frey's original
- [Riecoin Core](https://github.com/RiecoinTeam/Riecoin) — Our base (v2511)
- [LWMA Difficulty](https://github.com/zawy12/difficulty-algorithms) — zawy12's algorithms
- [Bitcoin Core](https://github.com/bitcoin/bitcoin) — Upstream reference
