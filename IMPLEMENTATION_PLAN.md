# Freycoin Implementation Plan

**Base:** Riecoin Core v2511 (Bitcoin Core 30.0, C++20, CMake, Qt6, GMP)
**Branch:** `freycoin-dev`
**Goal:** Replace Riecoin's constellation PoW with prime gap PoW. Testnet first.

The existing codebase finds prime constellations (primes close together). We replace that with prime gap mining (primes far apart). The P2P, wallet, RPC, and build infrastructure stays — only the PoW layer changes.

---

## Source Code Inventory (What We're Porting)

### From Gapcoin-Revival/src/PoWCore/ (~15,000 lines)

| File | Lines | Purpose |
|------|-------|---------|
| `PoW.h/cpp` | ~200 + 600 | PoW object: validate gaps, compute merit/difficulty |
| `PoWUtils.h/cpp` | ~400 + 1,500 | Fixed-point math (2^48), difficulty, merit, target size |
| `PoWProcessor.h` | ~50 | Abstract callback interface |
| `MiningEngine.h/cpp` | ~600 + 4,500 | High-level mining: tiers, pipeline, workers, GPU queue |
| `Sieve.h/cpp` | ~150 + 1,300 | Legacy sieve (Fermat test, gap validation) |
| `SieveOptimized.cpp` | ~1,800 | Wheel factorization sieve |
| `SegmentedSieve.h` | ~300 | L2-cache-optimized sieve |
| `CombinedSieve.h/cpp` | ~250 + 2,000 | Presieve + small prime + bucket algorithm |
| `SimdPresieve.h/cpp` | ~300 + 2,700 | AVX-512/AVX2/SSE2 presieve (primes 2-163) |
| `WheelTables.h` | ~800 | Wheel-2310 pre-computed residue tables |

### From Gapcoin-Revival/src/gpu/ (~8,000 lines)

| File | Lines | Purpose |
|------|-------|---------|
| `cuda_fermat.h` | ~50 | CUDA API: init, batch, cleanup, device detection |
| `fermat.cu` | ~1,300 | CUDA kernels: Montgomery 320/352-bit, Fermat test |
| `opencl_fermat.h` | ~50 | OpenCL API: init, batch, cleanup |
| `opencl_fermat.cpp` | ~340 | OpenCL runtime: context, queue, kernel management |
| `fermat.cl` | ~750 | OpenCL kernels: Montgomery, Fermat (3-bit window) |
| `fermat_cl_source.h` | ~5,800 | Embedded kernel as C string (generated) |

### From Gapcoin-Revival/src/qt/ (Mining GUI, ~2,000 lines)

| File | Lines | Purpose |
|------|-------|---------|
| `miningpage.h/cpp` | ~100 + ~500 | Mining controls: CPU/GPU, start/stop, stats |
| `miningpage.ui` | ~400 | Qt UI definition |
| `primerecordspage.h/cpp` | ~100 + ~500 | Blockchain gap records viewer |
| `primerecordspage.ui` | ~300 | Qt UI definition |

---

## Riecoin Core PoW Architecture (What We Replace)

### Key Files in Riecoin

- `src/pow.cpp` / `src/pow.h` — ~800 lines. Contains:
  - `DeriveTarget()` — converts nBits to GMP target
  - `CheckConstellation()` — validates prime tuples
  - Difficulty adjustment (ASERT post-Fork2)
  - Sieve of Eratosthenes for pre-filtering

- `src/primitives/block.h` — Block header structure:
  - `arith_uint256 nOffset` — 256-bit solution field
  - `GetHashForPoW()` — hash excluding nOffset

- `src/validation.cpp` — calls PoW validation hooks

- `src/chainparams.cpp` — consensus parameters, accepted patterns

### What Changes

| Riecoin Component | Freycoin Replacement |
|-------------------|---------------------|
| `nOffset` (256-bit, encodes primorial) | `nShift` (16-bit) + `nAdd` (256-bit) + `nDifficulty` (64-bit) |
| `nBits` (32-bit, compact target) | `nDifficulty` (64-bit, fixed-point merit target) |
| `DeriveTarget()` | Not needed (difficulty is direct) |
| `CheckConstellation()` | `CheckProofOfWork()` — gap validation |
| ASERT difficulty | LWMA difficulty |
| `mpz_probab_prime_p` (validation only) | Full mining engine + GPU |
| No built-in miner | Complete MiningEngine with GUI |

---

## Block Header Design

### Riecoin's Current Header (112 bytes)

```
nVersion          4B    int32_t
hashPrevBlock    32B    uint256
hashMerkleRoot   32B    uint256
nTime             8B    int64_t      ← 64-bit (overkill)
nBits             4B    uint32_t     ← compact difficulty
nNonce           32B    arith_uint256 ← 256-bit nonce/solution
```

### Freycoin's Header (120 bytes)

```
nVersion          4B    int32_t      Block version
hashPrevBlock    32B    uint256      Previous block hash
hashMerkleRoot   32B    uint256      Merkle root
nTime             4B    uint32_t     Timestamp (32-bit, sufficient until 2106)
nDifficulty       8B    uint64_t     Target difficulty (2^48 fixed-point)
nShift            2B    uint16_t     Left-shift for starting prime
nNonce            4B    uint32_t     Miner nonce (iterated)
nAdd             32B    uint256      Adder to form starting prime
nReserved         2B    uint16_t     Reserved / alignment
```

### Design Rationale

**Why 32-bit nTime (not Riecoin's 64-bit)?**
- Saves 4 bytes per header
- Sufficient until 2106 (unix timestamp overflow)
- Matches Bitcoin Core's battle-tested approach
- Can soft-fork to 64-bit in 80 years if needed

**Why 64-bit nDifficulty (not 32-bit nBits)?**
- Prime gap difficulty needs fractional precision
- 2^48 fixed-point gives 16 integer bits + 48 fractional bits
- Matches PoWCore's internal representation exactly
- No lossy compact encoding/decoding

**Why 256-bit nAdd (not variable-length)?**
- Fixed header size eliminates DoS vectors
- MAX_SHIFT=256 gives 512-bit starting primes (meaningful gaps)
- Riecoin proved this approach works in production
- Simplifies serialization, bandwidth estimation, caching

**Why separate nShift + nAdd (not single nOffset)?**
- nShift rarely changes between blocks (typically 14-256)
- Explicit separation makes the math clear: start = hash*2^shift + add
- Miners iterate nNonce and nAdd, not nShift
- PoW validation is more readable

**Why 4-byte nNonce (not 32-byte like Riecoin)?**
- Miners get 2^32 nonces per nAdd value
- If exhausted, change nAdd (which changes the search space)
- 4 bytes is standard Bitcoin convention
- Keeps header compact

---

## Difficulty Adjustment: LWMA

### Why LWMA over ASERT

**ASERT** assumes: doubling hashrate halves solve time (exponential relationship). This holds for SHA256 PoW where difficulty is a direct threshold on hash output.

**Prime gap PoW** is non-linear: increasing difficulty requires exponentially larger gaps, but the probability of finding them follows the Prime Number Theorem. The relationship between "effective hashrate" (primes/second) and expected block time at a given difficulty is NOT exponential.

**LWMA** makes no assumptions about the difficulty/time relationship. It simply observes recent block times and adjusts proportionally, weighted toward recent blocks. This robustness is why zawy12 designed it specifically for altcoins with unusual PoW.

### Parameters

```
N = 144          # Lookback window (144 blocks ≈ 6 hours at 150s)
T = 150          # Target spacing (seconds)
k = N*(N+1)*T/2  # Normalization constant

# For each of the last N blocks:
weighted_sum += solvetime[i] * weight[i]    # weight = position (1..N)
difficulty_sum += difficulty[i]

next_difficulty = difficulty_sum * T / weighted_sum

# Clamps:
next_difficulty = clamp(next_difficulty, prev/4, prev*4)
next_difficulty = max(next_difficulty, MIN_DIFFICULTY)
```

### Edge Cases

- **First N blocks:** Use minimum difficulty
- **Negative solvetimes:** Clamp to -6T (timestamps can go backwards slightly)
- **Extreme solvetimes:** Clamp to +6T
- **Hashrate crash:** 4x decrease per block, recovers in ~36 blocks
- **Hashrate spike:** 4x increase per block, stabilizes quickly

---

## Implementation Phases

### Phase 1: Rebrand & Build Verification

**Goal:** Renamed project that builds cleanly with no functional changes.

1. Global rename: Riecoin → Freycoin (all case variants)
2. Update CMakeLists.txt: project name, binary names
3. Update package metadata: version, copyright
4. Verify clean Linux build: `cmake -B build && cmake --build build`
5. Verify unit tests still pass

**Files touched:** ~100+ (mostly mechanical rename)
**Risk:** Low (no logic changes)

### Phase 2: Block Header Modification

**Goal:** New header structure compiles and serializes correctly.

6. Modify `src/primitives/block.h`:
   - Remove `arith_uint256 nOffset`
   - Add `uint64_t nDifficulty`, `uint16_t nShift`, `uint32_t nNonce`, `uint256 nAdd`
   - Change `nTime` from `int64_t` to `uint32_t`
   - Update `SERIALIZE_METHODS`
   - Implement `GetHash()` excluding proof fields

7. Update `src/chain.h` / `CBlockIndex`:
   - Store nDifficulty, nShift, nAdd in block index
   - Update disk serialization

8. Update `src/validation.cpp`:
   - Stub `CheckProofOfWork()` (accepts all blocks temporarily)
   - Wire up new header fields

9. Update `src/rpc/mining.cpp`:
   - `getblocktemplate` returns new fields
   - `submitblock` accepts new header format

10. Compile and verify: headers serialize/deserialize round-trip correctly

**Files touched:** ~15
**Risk:** Medium (serialization must be exact)

### Phase 3: Chain Parameters & Genesis

**Goal:** Testnet boots with genesis block.

11. Create testnet parameters in `src/chainparams.cpp`:
    - Network magic, ports, address prefixes
    - Initial difficulty (very low for testing)
    - Halving interval, block reward

12. Mine testnet genesis block:
    - Write standalone genesis miner (or use stub PoW)
    - Record genesis hash, merkle root, nonce

13. Set DNS seeds to empty (manual peer connection for testing)

14. Boot two nodes, verify they connect and sync genesis

**Files touched:** ~5
**Risk:** Low

### Phase 4: PoW Validation

**Goal:** `CheckProofOfWork()` correctly validates/rejects blocks.

15. Create `src/pow/` directory with CMakeLists.txt

16. Port `PoWUtils` (merit, difficulty, fixed-point math):
    - `GetGapMerit(mpz_class start, mpz_class end) → uint64_t`
    - `GetProofDifficulty(hash, shift, add) → uint64_t`
    - `GetTargetGapSize(start, difficulty) → uint64_t`
    - Fixed-point ln() using MPFR

17. Implement `CheckProofOfWork(CBlockHeader)`:
    - Validate nShift range (14-256)
    - Construct start = hash * 2^shift + add
    - Verify start is prime (mpz_probab_prime_p, 32 iterations)
    - Find gap (next prime after start)
    - Compute achieved difficulty
    - Compare against nDifficulty

18. Implement LWMA `GetNextWorkRequired()`:
    - Walk back N=144 blocks
    - Weighted solvetime sum
    - Compute next difficulty with clamps

19. Write unit tests:
    - Known valid/invalid blocks
    - Difficulty adjustment sequences
    - Merit calculation against reference values

20. Remove stub, wire real validation into `validation.cpp`

**Files touched:** ~10 new + ~5 modified
**Risk:** High (consensus-critical code, must be perfect)

### Phase 5: Mining Engine

**Goal:** `generateblocks` RPC mines valid blocks.

21. Port `SegmentedSieve`:
    - 32KB L1-cache segments
    - Small prime direct sieving
    - Candidate extraction

22. Port `CombinedSieve`:
    - Wheel-2310 filtering (480/2310 residues)
    - Bucket algorithm for large primes (Oliveira e Silva)
    - `WheelTables.h` pre-computed tables

23. Port `SimdPresieve`:
    - Runtime CPU detection (AVX-512, AVX2, SSE2, fallback)
    - 16 presieve tables for primes 2-163
    - Dispatch functions

24. Port `PrimalityTester`:
    - Fermat test (base 2)
    - Miller-Rabin (base 2)
    - Strong Lucas-Selfridge
    - BPSW composite (Fermat + Lucas)

25. Port `MiningEngine` + `MiningPipeline`:
    - Replace Boost threading with std::jthread
    - Sieve workers → candidate batches → gap detection
    - PoWProcessor callback interface
    - Statistics tracking (atomic counters)

26. Integrate with `src/miner.cpp` / mining RPC:
    - `generateblocks` creates header template
    - MiningEngine iterates nonces
    - Valid gaps submitted as blocks

27. Test: `freycoin-cli generateblocks 1` mines a block on testnet

**Files touched:** ~15 new
**Risk:** Medium (well-understood code being ported)

### Phase 6: GPU Acceleration

**Goal:** GPU mining produces valid blocks.

28. Create `src/gpu/` with CMakeLists.txt:
    - CUDA detection and compilation
    - OpenCL detection and linking
    - Conditional compilation (HAVE_CUDA, HAVE_OPENCL)

29. Port CUDA kernels (`fermat.cu`):
    - Montgomery multiplication 320/352-bit
    - Fermat primality test (5-bit window)
    - Kernel launch configuration

30. Port OpenCL runtime (`opencl_fermat.cpp`):
    - Platform/device detection
    - Context/queue/program management
    - Batch dispatch

31. Port OpenCL kernels (`fermat.cl`):
    - Montgomery multiplication
    - Fermat test (3-bit window)
    - Embedded source header

32. Integrate GPU worker thread in MiningPipeline:
    - CandidateBatch queue
    - GPU init/batch/cleanup lifecycle
    - Result processing → gap detection

33. Add MiningTier auto-detection:
    - Query CUDA devices
    - Query OpenCL devices
    - Select highest available tier

34. Test: GPU mining produces valid blocks faster than CPU-only

**Files touched:** ~10 new
**Risk:** Medium (hardware-dependent, needs real GPU to test)

### Phase 7: Qt6 Mining GUI

**Goal:** freycoin-qt has Mining and Records tabs.

35. Port `MiningPage`:
    - CPU thread slider/spinbox
    - GPU device dropdown + intensity
    - Solo/Pool toggle with pool fields
    - Start/Stop buttons
    - Real-time stats (primes/s, gaps, best merit)
    - Mining log with timestamps
    - Hardware auto-detection

36. Port `PrimeRecordsPage`:
    - Blockchain scan for high-merit gaps
    - Sortable table (Height, Date, Gap, Merit, Mine?, Start)
    - Statistics header
    - Refresh + CSV export
    - 30-second auto-update timer

37. Integrate into main window:
    - Add tabs to `FreycoinGUI`
    - Wire mining controls to `MiningEngine`
    - Settings persistence via `QSettings`

38. Adapt for Qt6:
    - Replace deprecated Qt5 APIs
    - Use Qt6 signal/slot syntax
    - Update .ui files for Qt6 Designer

**Files touched:** ~8 new + ~3 modified
**Risk:** Low (UI code, non-consensus)

### Phase 8: Testing & Validation

**Goal:** Multi-node testnet runs stably for 24+ hours.

39. Unit test suite:
    - PoW validation (valid/invalid blocks)
    - Difficulty adjustment (various scenarios)
    - Merit calculation (against reference)
    - Sieve correctness (known prime ranges)
    - Primality testing (known primes/composites)

40. Functional tests (Python):
    - `mining_basic.py` — mine blocks, verify chain
    - `mining_difficulty.py` — difficulty adjustment under load
    - `pow_validation.py` — reject invalid proofs
    - `wallet_mining.py` — coinbase maturity

41. Multi-node testnet:
    - 3+ nodes mining simultaneously
    - Verify consensus (same chain)
    - Test reorgs (stop/start miners)
    - Difficulty stabilization after hashrate changes

42. Windows build:
    - Cross-compile from Linux/WSL
    - Test freycoin-qt.exe on Windows
    - GPU mining on Windows (CUDA + OpenCL)

43. Stress testing:
    - Rapid mining (low difficulty)
    - Large mempool
    - Malformed block submissions (DoS)

**Files touched:** ~15 new test files
**Risk:** Low (testing, not production code)

### Phase 9: Polish

44. Icons, splash screen, branding assets
45. Documentation (README, build guides)
46. RPC help text updates
47. Error message cleanup
48. Release packaging (Guix reproducible builds)

---

## Success Criteria

The implementation is DONE when:

- [ ] Two testnet nodes mine and sync continuously for 24+ hours
- [ ] Average block time converges to 150s under LWMA
- [ ] CPU mining produces valid blocks
- [ ] CUDA GPU mining produces valid blocks
- [ ] OpenCL GPU mining produces valid blocks
- [ ] Windows freycoin-qt.exe starts, mines, sends/receives FREY
- [ ] Linux freycoind mines via RPC
- [ ] Mining page shows real-time statistics
- [ ] Prime records page displays blockchain gaps with correct merit
- [ ] All unit tests pass
- [ ] All functional tests pass
- [ ] No consensus failures or chain splits
- [ ] Difficulty recovers after hashrate drops to zero then resumes
- [ ] Coinbase maturity enforced (100 blocks)
- [ ] CSV export of prime records works

---

## Open Questions (To Resolve During Implementation)

1. **MPFR vs GMP for ln():** MPFR gives arbitrary precision ln(), but GMP can approximate via mpz_sizeinbase(). Trade-off: accuracy vs dependency count. Current plan: use MPFR.

2. **Boost elimination:** Riecoin Core still uses some Boost. How aggressively do we replace with C++20? Plan: replace in new code (PoWCore port), leave existing Bitcoin Core Boost usage alone.

3. **nReserved field usage:** 2 bytes reserved in header. Could encode mining metadata (GPU model, software version) but that's non-consensus. Leave as zero for now.

4. **Pool mining protocol:** Stratum v1 or v2? Current plan: implement solo mining first, add pool protocol later.

5. **Minimum difficulty for testnet:** How low? Needs to be mineable on a single CPU core in <10 seconds for testing. Will determine empirically.

---

## Risk Mitigation

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| GMP version mismatch | Low | Build fails | Pin in depends/ |
| CUDA SDK fragmentation | Medium | Some GPUs fail | Target sm_50+ (Maxwell+) |
| LWMA oscillation | Low | Unstable times | 4x clamp, tested on zawy12's testbed |
| nAdd overflow | None | Consensus bug | Hard MAX_SHIFT=256, validated in CheckProofOfWork |
| Riecoin upstream diverges | Low | Merge pain | Pinned to v2511, no tracking |
| Qt6 Windows build issues | Medium | No Windows GUI | Qt5 fallback, or static linking |
| MPFR precision edge cases | Low | Wrong merit | 256-bit precision, cross-check with Python mpmath |
| Performance regression in port | Medium | Slow mining | Benchmark PoWCore standalone first, then integrated |

---

## Appendix: PoW Mathematics

### Merit Formula

```
merit = gap_size / ln(start_prime)
```

Where `start_prime` is the first prime of the gap (the smaller one). Merit > 1.0 means the gap is larger than statistically expected for primes of that size (by Cramér's conjecture, gaps near p should be ~(ln p)^2, so merit ~ln(p) is "expected").

### Difficulty Encoding

```
difficulty = uint64_t, fixed-point with 48 fractional bits
real_difficulty = difficulty / 2^48

Example: difficulty = 0x0014_0000_0000_0000
         real = 20.0 (merit must exceed ~20)
```

### PoW Verification (Detailed)

```python
def check_pow(block):
    # 1. Hash the consensus fields (76 bytes)
    hash = sha256d(version + prev + merkle + time + difficulty + nonce)

    # 2. Construct starting number
    n = int(hash) * 2**shift + int(add)

    # 3. Verify n is prime
    assert is_prime(n)  # BPSW, 32 iterations

    # 4. Find next prime
    p2 = next_prime(n)

    # 5. Compute gap and merit
    gap = p2 - n
    merit = gap / ln(n)  # 2^48 fixed-point

    # 6. Compute difficulty with randomness
    # (prevents pre-computing solutions)
    rand = pseudo_random(n, p2)  # Deterministic from the gap
    achieved = merit + 2.0/ln(n) * rand

    # 7. Accept if sufficient
    return achieved >= block.difficulty
```

### Why the Randomness Term?

Without randomness, miners could pre-compute "which hash values lead to large gaps" and skip most nonces. The `random(p1, p2)` term, derived deterministically from both primes, ensures every nonce must be fully evaluated. The `2/ln(p)` scaling keeps the random contribution proportional to the difficulty scale.
