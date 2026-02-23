# Freycoin Core

Freycoin is a cryptocurrency whose Proof-of-Work discovers **prime gaps** — unusually large distances between consecutive prime numbers. Every block mined contributes to the mathematical frontier of prime gap research. Gaps discovered by miners earn places in the [Top-20 Prime Gaps](https://www.trnicely.net/gaps/gaplist.html) record lists maintained by mathematicians.

This is PoW with meaning — energy spent on number theory, not thrown into the void of meaningless hash collisions.

Freycoin is built on Bitcoin Core 30.0 (via Riecoin 2511) and inherits all of Bitcoin's battle-tested infrastructure: SegWit, Taproot, descriptor wallets, and the full UTXO-based transaction model.

**Website:** [freycoin.tech](https://freycoin.tech)
**Explorer:** [explorer.freycoin.tech](https://explorer.freycoin.tech) (mainnet) | [testnet.freycoin.tech](https://testnet.freycoin.tech) (testnet)

## How It Works

Miners search for large gaps between consecutive primes near numbers derived from block headers. The difficulty metric is **merit** = gap_size / ln(start_prime). High-merit gaps are genuine mathematical discoveries that advance human knowledge of prime distribution.

The mining engine uses:
- **Segmented sieve** with SIMD presieve (AVX-512/AVX2/SSE2) for candidate generation
- **BPSW primality testing** for start-of-gap verification
- **gwnum (FFT-based arithmetic)** for fast next-prime computation via George Woltman's number theory library
- **GPU acceleration** (OpenCL for AMD/Intel, CUDA PTX for NVIDIA) for batch Fermat primality testing

## Pre-Built Binaries

Most users should download the latest release binaries from the [Releases](https://github.com/shitcoinsherpa/Freycoin/releases) page. Available for Windows (x64) and Linux (x64).

## Building from Source

### Requirements

- **C++20 compiler:** GCC 13+ (recommended), Clang 17+, or MSVC 2022 17.6+
- **CMake** 3.22+
- **GMP** (arbitrary precision integer arithmetic for prime computations)
- **MPFR** (arbitrary precision floating point for ln() in merit calculation)
- **libevent** 2.1.8+
- **SQLite3** 3.7.17+
- **Qt 6.2+** (optional, for GUI wallet)
- **Boost** (headers only)

### Bundled Libraries

The source tree includes several bundled libraries that do not need to be installed separately:

- **`src/gwnum/`** — George Woltman's FFT-based modular arithmetic library (from GIMPS/Prime95). Provides the `fast_nextprime` function that accelerates prime gap verification by ~3x over GMP's `mpz_nextprime`. Includes pre-assembled x86-64 objects for FFT routines (Linux ELF and Windows COFF) plus C/C++ sources compiled at build time. x86-64 only; other architectures fall back to GMP.

- **`src/gpu/cgbn_lib/`** — Cooperative Groups Big Number library (CGBN) for GPU-accelerated big number arithmetic used in CUDA Fermat primality testing.

### Linux (Ubuntu 24.04 — Recommended)

```bash
sudo apt install build-essential cmake pkg-config \
  libgmp-dev libmpfr-dev libevent-dev libsqlite3-dev \
  libboost-dev qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev

git clone https://github.com/shitcoinsherpa/Freycoin.git
cd Freycoin
cmake -B build -DBUILD_GUI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binaries are in `build/bin/`. Run `strip build/bin/freycoin-qt` to reduce size.

To build without the GUI:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Linux (Ubuntu 22.04 / WSL2)

Ubuntu 22.04's GCC 11 is too old for C++20. Use Clang 17 with libc++:

```bash
sudo apt install build-essential cmake pkg-config \
  libgmp-dev libmpfr-dev libevent-dev libsqlite3-dev libboost-dev libzmq3-dev

# Install Clang 17
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
echo "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-17 main" | sudo tee /etc/apt/sources.list.d/llvm-17.list
sudo apt update && sudo apt install -y clang-17 lld-17 libc++-17-dev libc++abi-17-dev

CC=clang-17 CXX=clang++-17 cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-stdlib=libc++' \
  -DCMAKE_EXE_LINKER_FLAGS='-stdlib=libc++ -lc++abi' \
  -DBUILD_GUI=OFF
cmake --build build -j$(nproc)
```

Note: Qt6 GUI is not available on Ubuntu 22.04 with this method (Qt6 is built against libstdc++, incompatible with libc++). Use Ubuntu 24.04 for GUI builds.

### Windows Cross-Compilation (from Ubuntu 24.04)

This is the recommended method for Windows binaries with Qt6 GUI:

```bash
sudo apt install build-essential cmake pkg-config python3 \
  g++-mingw-w64-x86-64-posix mingw-w64-tools nsis \
  libgmp-dev libmpfr-dev curl

git clone https://github.com/shitcoinsherpa/Freycoin.git
cd Freycoin/depends
make HOST=x86_64-w64-mingw32 -j4    # 30-60 minutes

cd ..
cmake -B build-win --toolchain depends/x86_64-w64-mingw32/toolchain.cmake
cmake --build build-win -j$(nproc)
```

### Windows Native (MSVC)

Requires Visual Studio 2022 with vcpkg. Qt6 has parallel build issues on Windows — use single-threaded compilation:

```powershell
$env:VCPKG_MAX_CONCURRENCY = "1"
cmake -B build --preset vs2022-static
cmake --build build --config Release -j1
```

## Chain Parameters

| Parameter | Mainnet | Testnet |
|-----------|---------|---------|
| Block time | 150 seconds | 150 seconds |
| Initial reward | 50 FREY | 50 FREY |
| Halving interval | 840,000 blocks (~4 years) | 840,000 blocks |
| Tail emission | 0.1 FREY perpetual | 0.1 FREY |
| Coinbase maturity | 100 blocks | 100 blocks |
| Default P2P port | 31470 | 31473 |
| Default RPC port | 31469 | 31472 |

## Testing

```bash
build/bin/test_freycoin                        # Unit tests
build/test/functional/test_runner.py           # Functional tests (-j N for parallel)
build/bin/test_freycoin-qt                     # Qt GUI tests
```

## In Memory Of

Jonnie Frey (1989-2017) created Gapcoin — the first cryptocurrency to use prime gaps as Proof-of-Work. He died too young, but his vision that mining should produce scientific value lives on in Freycoin.

## License

Freycoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for details.

Releases are distributed under GPLv3 due to inclusion of GPL-licensed dependencies.
