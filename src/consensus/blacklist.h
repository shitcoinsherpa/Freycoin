// Copyright (c) 2025-present The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FREYCOIN_CONSENSUS_BLACKLIST_H
#define FREYCOIN_CONSENSUS_BLACKLIST_H

#include <kernel/chainparams.h>

using namespace util::hex_literals;

// Freycoin blacklist — empty for new chain.
// Add entries here if exchange exit scams or similar theft occurs.
static const Blacklist blacklist = {
	{}
};

#endif // FREYCOIN_CONSENSUS_BLACKLIST_H
