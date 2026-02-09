# Freycoin Core

Freycoin is a cryptocurrency whose Proof-of-Work discovers prime gaps — distances between consecutive prime numbers that contribute to number theory research. Every block mined is a genuine mathematical computation, not a meaningless hash collision.

*In memory of Jonnie Frey (1989-2017), creator of Gapcoin, who proved that PoW can advance human knowledge.*

## Overview

- **Proof-of-Work:** Miners search for large gaps between consecutive primes. Difficulty is measured by merit = gap_size / ln(start_prime).
- **Base:** Bitcoin Core 30.0 (SegWit, Taproot, descriptor wallets)
- **Block time:** 150 seconds
- **Initial reward:** 50 FREY, halving every 840,000 blocks (~4 years)
- **Tail emission:** 0.1 FREY perpetual floor
- **GPU acceleration:** OpenCL Fermat primality pre-filter

## Build

### Linux (Ubuntu 24.04)

```bash
sudo apt install build-essential cmake pkg-config \
  libgmp-dev libmpfr-dev libevent-dev libsqlite3-dev \
  qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev

git clone https://github.com/shitcoinsherpa/Freycoin.git
cd Freycoin
cmake -B build -DBUILD_GUI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binaries are in `build/bin/`.

### Linux (Ubuntu 22.04)

Ubuntu 22.04 requires Clang 17 with libc++ (GCC 11 lacks C++20 support). GUI cannot be built on 22.04.

```bash
sudo apt install build-essential cmake pkg-config \
  libgmp-dev libmpfr-dev libevent-dev libsqlite3-dev

# Install Clang 17
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
echo "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-17 main" | sudo tee /etc/apt/sources.list.d/llvm-17.list
sudo apt update && sudo apt install clang-17 lld-17 libc++-17-dev libc++abi-17-dev

CC=clang-17 CXX=clang++-17 cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-stdlib=libc++' \
  -DCMAKE_EXE_LINKER_FLAGS='-stdlib=libc++ -lc++abi' \
  -DBUILD_GUI=OFF
cmake --build build -j$(nproc)
```

### Windows Cross-Compilation (from Ubuntu 24.04)

```bash
sudo apt install g++-mingw-w64-x86-64-posix mingw-w64-tools

cd Freycoin/depends
make HOST=x86_64-w64-mingw32 -j4

cd ..
cmake -B build-win --toolchain depends/x86_64-w64-mingw32/toolchain.cmake
cmake --build build-win -j$(nproc)
```

### Dependencies

| Dependency | Purpose |
|-----------|---------|
| GMP | Arbitrary precision integer arithmetic |
| MPFR | Arbitrary precision floating point (merit calculation) |
| libevent | Networking |
| SQLite3 | Descriptor wallet storage |
| Qt 6.2+ | GUI wallet (optional) |

## Testing

```bash
build/bin/test_freycoin
python3 test/functional/test_runner.py
```

## Links

- Website: https://freycoin.tech
- Explorer: https://explorer.freycoin.tech
- Testnet Explorer: https://testnet.freycoin.tech

## License

Freycoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for more information.
