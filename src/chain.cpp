// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2013-present The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/time.h>

std::string CBlockFileInfo::ToString() const
{
    return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst, nHeightLast, FormatISO8601Date(nTimeFirst), FormatISO8601Date(nTimeLast));
}

std::string CBlockIndex::ToString() const
{
    return strprintf("CBlockIndex(pprev=%p, nHeight=%d, merkle=%s, hashBlock=%s)",
                     pprev, nHeight, hashMerkleRoot.ToString(), GetBlockHash().ToString());
}

void CChain::SetTip(CBlockIndex& block)
{
    CBlockIndex* pindex = &block;
    vChain.resize(pindex->nHeight + 1);
    while (pindex && vChain[pindex->nHeight] != pindex) {
        vChain[pindex->nHeight] = pindex;
        pindex = pindex->pprev;
    }
}

std::vector<uint256> LocatorEntries(const CBlockIndex* index)
{
    int step = 1;
    std::vector<uint256> have;
    if (index == nullptr) return have;

    have.reserve(32);
    while (index) {
        have.emplace_back(index->GetBlockHash());
        if (index->nHeight == 0) break;
        // Exponentially larger steps back, plus the genesis block.
        int height = std::max(index->nHeight - step, 0);
        // Use skiplist.
        index = index->GetAncestor(height);
        if (have.size() > 10) step *= 2;
    }
    return have;
}

CBlockLocator GetLocator(const CBlockIndex* index)
{
    return CBlockLocator{LocatorEntries(index)};
}

const CBlockIndex *CChain::FindFork(const CBlockIndex *pindex) const {
    if (pindex == nullptr) {
        return nullptr;
    }
    if (pindex->nHeight > Height())
        pindex = pindex->GetAncestor(Height());
    while (pindex && !Contains(pindex))
        pindex = pindex->pprev;
    return pindex;
}

CBlockIndex* CChain::FindEarliestAtLeast(int64_t nTime, int height) const
{
    std::pair<int64_t, int> blockparams = std::make_pair(nTime, height);
    std::vector<CBlockIndex*>::const_iterator lower = std::lower_bound(vChain.begin(), vChain.end(), blockparams,
        [](CBlockIndex* pBlock, const std::pair<int64_t, int>& blockparams) -> bool { return pBlock->GetBlockTimeMax() < blockparams.first || pBlock->nHeight < blockparams.second; });
    return (lower == vChain.end() ? nullptr : *lower);
}

/** Turn the lowest '1' bit in the binary representation of a number into a '0'. */
int static inline InvertLowestOne(int n) { return n & (n - 1); }

/** Compute what height to jump back to with the CBlockIndex::pskip pointer. */
int static inline GetSkipHeight(int height) {
    if (height < 2)
        return 0;

    // Determine which height to jump back to. Any number strictly lower than height is acceptable,
    // but the following expression seems to perform well in simulations (max 110 steps to go back
    // up to 2**18 blocks).
    return (height & 1) ? InvertLowestOne(InvertLowestOne(height - 1)) + 1 : InvertLowestOne(height);
}

const CBlockIndex* CBlockIndex::GetAncestor(int height) const
{
    if (height > nHeight || height < 0) {
        return nullptr;
    }

    const CBlockIndex* pindexWalk = this;
    int heightWalk = nHeight;
    while (heightWalk > height) {
        int heightSkip = GetSkipHeight(heightWalk);
        int heightSkipPrev = GetSkipHeight(heightWalk - 1);
        if (pindexWalk->pskip != nullptr &&
            (heightSkip == height ||
             (heightSkip > height && !(heightSkipPrev < heightSkip - 2 &&
                                       heightSkipPrev >= height)))) {
            // Only follow pskip if pprev->pskip isn't better than pskip->pprev.
            pindexWalk = pindexWalk->pskip;
            heightWalk = heightSkip;
        } else {
            assert(pindexWalk->pprev);
            pindexWalk = pindexWalk->pprev;
            heightWalk--;
        }
    }
    return pindexWalk;
}

CBlockIndex* CBlockIndex::GetAncestor(int height)
{
    return const_cast<CBlockIndex*>(static_cast<const CBlockIndex*>(this)->GetAncestor(height));
}

void CBlockIndex::BuildSkip()
{
    if (pprev)
        pskip = pprev->GetAncestor(GetSkipHeight(nHeight));
}

/**
 * Compute chain work for a block.
 *
 * For prime gap PoW, the expected work to find a gap with difficulty D is approximately e^D
 * where D is the merit (nDifficulty / 2^48). Since e^D grows extremely fast, we use
 * 2^(D * log2(e)) = 2^(D * 1.4427...) ≈ 2^(3*D/2) as an integer approximation.
 *
 * This means each +1 increase in merit roughly triples the work (since 2^1.44 ≈ 2.7).
 * The approximation preserves ordering: higher difficulty = more work.
 */
arith_uint256 GetBlockProof(const CBlockIndex& block)
{
    // Convert fixed-point difficulty to approximate work
    // nDifficulty is 2^48 fixed-point, so real merit = nDifficulty >> 48
    // Work ≈ e^merit ≈ 2^(merit * 1.44) ≈ 2^(nDifficulty >> 47) for a rough approximation
    //
    // Use a shift that keeps values reasonable in 256 bits:
    // merit 20 → 2^29 ≈ 500M work units
    // merit 30 → 2^43 ≈ 8T work units
    // merit 40 → 2^58 work units
    uint64_t shiftAmount = block.nDifficulty >> 47;  // Roughly 1.5x the integer merit
    if (shiftAmount > 250) shiftAmount = 250;  // Prevent overflow

    arith_uint256 work = arith_uint256(1) << static_cast<unsigned int>(shiftAmount);
    return work;
}

int64_t GetBlockProofEquivalentTime(const CBlockIndex& to, const CBlockIndex& from, const CBlockIndex& tip, const Consensus::Params& params)
{
    arith_uint256 r;
    int sign = 1;
    if (to.nChainWork > from.nChainWork) {
        r = to.nChainWork - from.nChainWork;
    } else {
        r = from.nChainWork - to.nChainWork;
        sign = -1;
    }
    r = r * arith_uint256(params.nPowTargetSpacing) / GetBlockProof(tip);
    if (r.bits() > 63) {
        return sign * std::numeric_limits<int64_t>::max();
    }
    return sign * int64_t(r.GetLow64());
}

/** Find the last common ancestor two blocks have.
 *  Both pa and pb must be non-nullptr. */
const CBlockIndex* LastCommonAncestor(const CBlockIndex* pa, const CBlockIndex* pb) {
    // First rewind to the last common height (the forking point cannot be past one of the two).
    if (pa->nHeight > pb->nHeight) {
        pa = pa->GetAncestor(pb->nHeight);
    } else if (pb->nHeight > pa->nHeight) {
        pb = pb->GetAncestor(pa->nHeight);
    }
    while (pa != pb) {
        // Jump back until pa and pb have a common "skip" ancestor.
        while (pa->pskip != pb->pskip) {
            // This logic relies on the property that equal-height blocks have equal-height skip
            // pointers.
            Assume(pa->nHeight == pb->nHeight);
            Assume(pa->pskip->nHeight == pb->pskip->nHeight);
            pa = pa->pskip;
            pb = pb->pskip;
        }
        // At this point, pa and pb are different, but have equal pskip. The forking point lies in
        // between pa/pb on the one end, and pa->pskip/pb->pskip on the other end.
        pa = pa->pprev;
        pb = pb->pprev;
    }
    return pa;
}
