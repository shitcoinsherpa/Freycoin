#!/usr/bin/env python3
"""
Extract MAX_HEADERS_RESULTS-sized header batches from a synced freycoind
and emit C++ struct literals for src/kernel/checkpointdata.h.

Usage:
    extract_header_batches.py <freycoin-cli-cmd> <output.h>
where <freycoin-cli-cmd> is the full freycoin-cli prefix (rpc port + cookie).
"""
import hashlib
import subprocess
import sys

BATCH = 2000
SAFETY_MARGIN = 2000

def cli(cmd_prefix, *args):
    res = subprocess.run(cmd_prefix + list(args), capture_output=True, check=True, text=True)
    return res.stdout.strip()

def hash256(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()

def main():
    cli_cmd = sys.argv[1].split()
    out_path = sys.argv[2]

    tip_height = int(cli(cli_cmd, 'getblockcount'))
    last_safe_end = ((tip_height - SAFETY_MARGIN) // BATCH) * BATCH
    if last_safe_end < BATCH:
        sys.exit(f"Chain too short: tip={tip_height}, need at least {BATCH + SAFETY_MARGIN}")

    print(f"tip={tip_height}, last_safe_batch_end={last_safe_end}, batches={last_safe_end // BATCH}", file=sys.stderr)

    batches = []
    for batch_start in range(1, last_safe_end + 1, BATCH):
        batch_end = batch_start + BATCH - 1
        print(f"  batch height={batch_start}..{batch_end}", file=sys.stderr)
        concat = b''
        for h in range(batch_start, batch_end + 1):
            blockhash = cli(cli_cmd, 'getblockhash', str(h))
            header_hex = cli(cli_cmd, 'getblockheader', blockhash, 'false')
            concat += bytes.fromhex(header_hex)
        digest = hash256(concat)
        # Bitcoin convention: hashes display as reversed bytes (big-endian print)
        digest_be = digest[::-1].hex()
        batches.append((digest_be, batch_start, BATCH))

    # assumedValidBlock is the last block of the last batch
    avb_height = last_safe_end
    avb_hash = cli(cli_cmd, 'getblockhash', str(avb_height))

    out = []
    out.append('// Copyright (c) 2025-present The Freycoin developers')
    out.append('// Distributed under the MIT software license, see the accompanying')
    out.append('// file COPYING or http://www.opensource.org/licenses/mit-license.php.')
    out.append('')
    out.append('#ifndef FREYCOIN_KERNEL_CHECKPOINTDATA_H')
    out.append('#define FREYCOIN_KERNEL_CHECKPOINTDATA_H')
    out.append('')
    out.append('#include <kernel/chainparams.h>')
    out.append('')
    out.append('/**')
    out.append(' * Freycoin Checkpoint Data')
    out.append(' *')
    out.append(' * Each entry maps Hash(serialized_2000_headers) to (start_height, batch_size).')
    out.append(' * During initial sync, peers must feed full 2000-header batches whose hash')
    out.append(' * matches one of these entries; matching batches skip per-header PoW (which')
    out.append(' * costs ~30 s/header at mainnet shift). Above assumedValidBlockHeight,')
    out.append(' * normal headers-first sync applies.')
    out.append(' *')
    out.append(' * REGENERATE THIS FILE EACH RELEASE via scripts/extract_header_batches.py.')
    out.append(' */')
    out.append('')
    out.append('static const CheckpointData mainCheckpointData = {')
    out.append('    .knownHeaderBatchesHashes = {')
    for digest_be, start, size in batches:
        out.append(f'        {{ uint256{{"{digest_be}"}}, {{ {start}, {size} }} }},')
    out.append('    },')
    out.append(f'    .assumedValidBlockHash = uint256{{"{avb_hash}"}},')
    out.append(f'    .assumedValidBlockHeight = {avb_height}')
    out.append('};')
    out.append('')
    out.append('static const CheckpointData testCheckpointData = {')
    out.append('    .knownHeaderBatchesHashes = {},  // testnet — refresh separately when needed')
    out.append('    .assumedValidBlockHash = uint256{},')
    out.append('    .assumedValidBlockHeight = 0')
    out.append('};')
    out.append('')
    out.append('#endif // FREYCOIN_KERNEL_CHECKPOINTDATA_H')

    with open(out_path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print(f"wrote {out_path} with {len(batches)} batches, AVB height={avb_height}", file=sys.stderr)

if __name__ == '__main__':
    main()
