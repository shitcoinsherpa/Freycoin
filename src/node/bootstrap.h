// Copyright (c) 2026 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FREYCOIN_NODE_BOOTSTRAP_H
#define FREYCOIN_NODE_BOOTSTRAP_H

#include <util/fs.h>

#include <functional>
#include <optional>
#include <string>

class ArgsManager;

namespace node {

/**
 * Sync-from-bootstrap engine.
 *
 * Two-phase user-triggered operation:
 *   Phase A — pre-shutdown: download tarball + SHA sidecar from a hardcoded
 *             URL in chainparams, verify SHA256, write a marker file, request
 *             clean daemon shutdown.
 *   Phase B — startup-time: DataDirPreflight detects the marker, backs up
 *             existing blocks/chainstate/indexes, allowlist-extracts the
 *             staged tarball, deletes marker. Daemon then loads its new
 *             chainstate normally.
 *
 * Wallet, peers.dat, freycoin.conf, and every other operator artifact are
 * never touched. The extractor enforces a regex allowlist:
 *   ^(blocks|chainstate|indexes)/[^/]+(/[^/]+)*$
 * No `..`, no leading `/`, no symlinks. A malicious tarball with a
 * `wallets/wallet.dat` entry is rejected before any write.
 */

enum class BootstrapPhase {
    NONE,          // No marker, no operation pending
    DOWNLOADING,   // Tarball download in progress
    DOWNLOADED,    // Bytes on disk, SHA verified, marker written
    EXTRACTING,    // Mid-extract — recovery may be needed
    COMPLETE,      // Operation successful, marker about to be removed
};

struct BootstrapStatus {
    BootstrapPhase phase{BootstrapPhase::NONE};
    int64_t bytes_downloaded{0};
    int64_t bytes_total{0};
    std::string url;
    std::string expected_sha256;
    std::string error;            // Populated on failure
    fs::path staging_path;        // Where the tarball lives on disk
    fs::path marker_path;         // <datadir>/.bootstrap/pending.json
    fs::path backup_path;         // <datadir>/.bootstrap-backup/<ts>/
};

/**
 * Phase A: stage a bootstrap tarball.
 *
 * Downloads the tarball + SHA sidecar to <datadir>/.bootstrap/, verifies
 * the SHA, writes the pending.json marker, and returns. Caller is expected
 * to then call StartShutdown() — actual extraction happens on next start.
 *
 * @param datadir       Net-specific datadir (e.g. <root>/mainnet)
 * @param url           Tarball URL (typically chainparams.BootstrapURL())
 * @param progress_cb   Called periodically with (bytes_downloaded, bytes_total)
 *                      Pass nullptr to skip progress notifications.
 * @return              ok status or error reason
 */
[[nodiscard]] BootstrapStatus StageBootstrap(const fs::path& datadir,
                                              const std::string& url,
                                              std::function<void(int64_t, int64_t)> progress_cb = nullptr);

/**
 * Phase B: at startup, before LoadChainstate, look for a pending marker.
 * If present:
 *   - Move existing blocks/, chainstate/, indexes/ to .bootstrap-backup/<ts>/
 *   - Allowlist-extract staged tarball
 *   - Delete marker + staging
 *   - Return true (caller proceeds with normal startup using new chainstate)
 *
 * On any failure: roll back from backup, return false, daemon aborts startup.
 *
 * Called from DataDirPreflight().
 *
 * @param datadir   Net-specific datadir
 * @return          true if no operation needed OR successful extract;
 *                  false if pending marker exists but extract failed
 */
[[nodiscard]] bool MaybeApplyStagedBootstrap(const fs::path& datadir);

/**
 * Read the current bootstrap state from <datadir>/.bootstrap/pending.json.
 * Returns std::nullopt if no marker exists.
 */
std::optional<BootstrapStatus> ReadBootstrapMarker(const fs::path& datadir);

/**
 * Clean up successful or stale bootstrap state. Deletes the marker, the
 * staging tarball, and old .bootstrap-backup/<ts>/ entries older than the
 * retention threshold (~30 days).
 */
void CleanupBootstrapState(const fs::path& datadir);

} // namespace node

#endif // FREYCOIN_NODE_BOOTSTRAP_H
