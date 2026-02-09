# Freycoin Core

**Proof-of-Work that advances mathematics.**

Freycoin is a cryptocurrency whose mining algorithm discovers [prime gaps](https://en.wikipedia.org/wiki/Prime_gap) — unusually large distances between consecutive prime numbers. Every block mined contributes to the mathematical frontier of prime gap research. Record-worthy gaps earn places in the [Top-20 Prime Gaps](https://www.trnicely.net/gaps/gaplist.html) lists maintained by number theorists worldwide.

Built on Bitcoin Core 30.0. SegWit, Taproot, descriptor wallets — all the modern infrastructure, with useful work instead of meaningless hash collisions.

**Website:** [freycoin.tech](https://freycoin.tech)
**Mainnet Explorer:** [explorer.freycoin.tech](https://explorer.freycoin.tech)
**Testnet Explorer:** [testnet.freycoin.tech](https://testnet.freycoin.tech)

## How Mining Works

```
1. Construct a large number from the block header: start = SHA256d(header) * 2^shift + adder
2. Prove "start" is prime (BPSW primality test)
3. Find the next prime after "start"
4. The gap between them is the miner's proof-of-work
5. Merit = gap_size / ln(start) — larger gaps relative to the number size score higher
6. Block accepted if merit meets the network difficulty target
```

Miners compete to find the largest prime gaps they can. The bigger the gap relative to the size of the prime, the more "meritorious" the discovery — and the more likely it satisfies the difficulty target.

## Chain Parameters

| Parameter | Mainnet | Testnet |
|-----------|---------|---------|
| Block time | 150 seconds | 150 seconds |
| Initial reward | 50 FREY | 50 FREY |
| Halving interval | 840,000 blocks (~4 years) | 840,000 blocks |
| Tail emission | 0.1 FREY perpetual | 0.1 FREY perpetual |
| Coinbase maturity | 100 blocks | 100 blocks |
| P2P port | 31470 | 31473 |
| RPC port | 31469 | 31472 |
| Address prefix | `F` (base58) | `tfrey` (bech32) |

## Building from Source

### Requirements

- C++20 compiler (GCC 13+, Clang 17+, or MSVC 2022 17.6+)
- CMake 3.22+
- GMP (arbitrary precision arithmetic)
- MPFR (arbitrary precision floating-point)
- Libevent 2.1.8+
- SQLite3 3.7.17+
- Qt 6.2+ (optional, for GUI wallet)

### Linux (Ubuntu 24.04)

```bash
# Install dependencies
sudo apt install build-essential cmake pkg-config \
  libgmp-dev libmpfr-dev libevent-dev libsqlite3-dev libboost-dev \
  qt6-base-dev qt6-tools-dev qt6-l10n-tools libqrencode-dev

# Build
git clone https://github.com/shitcoinsherpa/Freycoin.git
cd Freycoin
cmake -B build -DBUILD_GUI=ON
cmake --build build -j$(nproc)
```

Binaries are in `build/bin/`. Run `strip build/bin/freycoin-qt` to reduce size.

### Windows Cross-Compilation (from Ubuntu 24.04)

This is the recommended method for Windows binaries with Qt6 GUI.

```bash
# Install cross-compile toolchain
sudo apt install g++-mingw-w64-x86-64-posix mingw-w64-tools

# Build dependencies (30-60 minutes)
cd depends
make HOST=x86_64-w64-mingw32 -j4

# Build Freycoin
cd ..
cmake -B build-win --toolchain depends/x86_64-w64-mingw32/toolchain.cmake
cmake --build build-win -j$(nproc)
```

### Build Targets

| Binary | Description |
|--------|-------------|
| `freycoin-qt` | GUI wallet |
| `freycoind` | Headless daemon |
| `freycoin-cli` | RPC command-line client |
| `freycoin-tx` | Transaction utility |
| `freycoin-wallet` | Offline wallet utility |

## Running

```bash
# Start the daemon
freycoind -daemon

# Mine with 4 threads
freycoind -gen -genproclimit=4

# Check status
freycoin-cli getmininginfo
freycoin-cli getblockchaininfo

# GUI wallet
freycoin-qt
```

## Testing

```bash
# Unit tests
cmake --build build --target test_freycoin
build/bin/test_freycoin

# Functional tests
python3 test/functional/test_runner.py
```

## In Memory of Jonnie Frey

Jonnie Frey (1989-2017) created [Gapcoin](https://github.com/nicehash/gapcoin), the first cryptocurrency to use prime gap discovery as proof-of-work. He proved that computational work securing a blockchain can simultaneously advance human knowledge. Freycoin carries his vision forward.

## License

Freycoin Core is released under the terms of the MIT license. See [COPYING](COPYING) for details.

Releases are distributed under GPLv3 due to GMP and MPFR dependencies.
