// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_MINING_H
#define BITCOIN_RPC_MINING_H

/** Default max iterations to try in RPC generatetodescriptor, generatetoaddress, and generateblock.
 * One try = one sieve segment (~262144 sieved positions); 64 segments is
 * about a minute of CPU mining per call, so callers loop and pick up a
 * fresh template each round instead of holding an HTTP worker. */
static const uint64_t DEFAULT_MAX_TRIES{64};

/** Per-round segment budget inside one GenerateBlock call. Long maxtries
 * values are split into rounds so the template (and nTime) is rebuilt
 * between rounds. */
static const uint64_t MINING_SEGMENTS_PER_ROUND{64};

#endif // BITCOIN_RPC_MINING_H
