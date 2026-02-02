// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Callback interface for processing discovered PoW solutions.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_POW_PROCESSOR_H
#define FREYCOIN_POW_PROCESSOR_H

class PoW;

/**
 * Abstract interface for processing discovered PoW solutions.
 *
 * The mining engine calls process() when a valid prime gap is found
 * that meets the difficulty target. The implementation should:
 *   1. Validate the PoW (optional, for paranoia)
 *   2. Construct a block with the PoW fields
 *   3. Submit the block to the network
 *
 * Return value:
 *   true  = continue mining (same nonce, searching for better gap)
 *   false = stop mining (solution accepted, get new work)
 */
class PoWProcessor {
public:
    PoWProcessor() = default;
    virtual ~PoWProcessor() = default;

    /**
     * Process a discovered PoW solution.
     *
     * @param pow The discovered proof-of-work
     * @return true to continue mining, false to stop and get new work
     */
    virtual bool process(PoW* pow) = 0;
};

#endif // FREYCOIN_POW_PROCESSOR_H
