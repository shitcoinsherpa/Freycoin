#!/bin/bash
# Auto-generate bootstrap snapshots every 100 blocks.
#
# Designed to run via cron every hour:
#   0 * * * * /opt/freycoin-src/contrib/auto-bootstrap.sh >> /var/log/freycoin-bootstrap.log 2>&1
#
# Checks if chain height has crossed a new 100-block boundary since the last
# snapshot. If so, stops the node, creates a clean archive, and restarts.

set -uo pipefail

DATADIR="/var/lib/freycoin/mainnet"
WEBDIR="/var/www/freycoin.tech/bootstrap"
RPC_USER="explorer"
RPC_PASS="b4b6c32b1a868f77adb0c722186e91bf60f4a9544f0c4b48"
RPC_PORT="31469"
SERVICE="freycoind-mainnet"
LOCKFILE="/tmp/bootstrap-generation.lock"
MAX_SNAPSHOTS=3

rpc() {
    freycoin-cli -rpcuser="$RPC_USER" -rpcpassword="$RPC_PASS" -rpcport="$RPC_PORT" "$@" 2>/dev/null
}

log() {
    echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*"
}

# Prevent concurrent runs
if [ -f "$LOCKFILE" ]; then
    LOCK_AGE=$(( $(date +%s) - $(stat -c%Y "$LOCKFILE") ))
    if [ "$LOCK_AGE" -lt 3600 ]; then
        log "Another bootstrap generation is running (lock age: ${LOCK_AGE}s). Skipping."
        exit 0
    fi
    log "Stale lock file (${LOCK_AGE}s old). Removing."
    rm -f "$LOCKFILE"
fi

# Get current height
HEIGHT=$(rpc getblockcount)
if [ -z "$HEIGHT" ] || [ "$HEIGHT" = "0" ]; then
    log "ERROR: Could not get block height from RPC. Node may be down."
    exit 1
fi

# Round down to nearest 100
SNAP_HEIGHT=$(( (HEIGHT / 100) * 100 ))

# Check last bootstrap height
LAST_HEIGHT=0
if [ -f "${WEBDIR}/bootstrap.json" ]; then
    LAST_HEIGHT=$(python3 -c "import json; print(json.load(open('${WEBDIR}/bootstrap.json'))['height'])" 2>/dev/null || echo 0)
fi

if [ "$SNAP_HEIGHT" -le "$LAST_HEIGHT" ]; then
    log "No new 100-block boundary. Current: $HEIGHT, last snapshot: $LAST_HEIGHT. Skipping."
    exit 0
fi

log "New bootstrap needed: height=$HEIGHT, snap_height=$SNAP_HEIGHT, last=$LAST_HEIGHT"
touch "$LOCKFILE"
trap 'rm -f "$LOCKFILE"' EXIT

# Stop the node
log "Stopping $SERVICE..."
systemctl stop "$SERVICE"

# Wait up to 5 minutes for clean shutdown
STOPPED=false
for i in $(seq 1 30); do
    if ! ps aux | grep -v grep | grep "freycoind.*mainnet" > /dev/null 2>&1; then
        STOPPED=true
        log "Node stopped cleanly after $((i * 10))s"
        break
    fi
    sleep 10
done

if [ "$STOPPED" = false ]; then
    log "Force-killing freycoind after 300s timeout..."
    ps aux | grep freycoind | grep -v grep | awk '{print $2}' | xargs kill -9 2>/dev/null
    sleep 3
fi

# Create the archive
FILENAME="freycoin-bootstrap-${SNAP_HEIGHT}.tar.gz"
log "Creating ${FILENAME}..."
cd "$DATADIR"
tar czf "/tmp/${FILENAME}" \
    --exclude="*/LOCK" \
    --exclude="*/LOG" \
    --exclude="*/LOG.old" \
    blocks/ chainstate/

SIZE_BYTES=$(stat -c%s "/tmp/${FILENAME}")
SIZE_HUMAN=$(numfmt --to=iec-i --suffix=B "$SIZE_BYTES" 2>/dev/null || echo "${SIZE_BYTES} bytes")
SHA256=$(sha256sum "/tmp/${FILENAME}" | awk '{print $1}')
CREATED=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

log "Archive: $FILENAME ($SIZE_HUMAN, sha256=$SHA256)"

# Deploy to web directory
mv "/tmp/${FILENAME}" "${WEBDIR}/${FILENAME}"
ln -sf "$FILENAME" "${WEBDIR}/latest.tar.gz"

# Build snapshots array (keep last MAX_SNAPSHOTS)
NEW_SNAP="{\"height\":${SNAP_HEIGHT},\"filename\":\"${FILENAME}\",\"size_bytes\":${SIZE_BYTES},\"size_human\":\"${SIZE_HUMAN}\",\"sha256\":\"${SHA256}\"}"

python3 -c "
import json, os

webdir = '${WEBDIR}'
max_snaps = ${MAX_SNAPSHOTS}
new_snap = json.loads('${NEW_SNAP}')

# Load existing snapshots
old_snaps = []
try:
    with open(os.path.join(webdir, 'bootstrap.json')) as f:
        old_snaps = json.load(f).get('snapshots', [])
except:
    pass

# Prepend new, keep last max_snaps
all_snaps = [new_snap] + [s for s in old_snaps if s['height'] != new_snap['height']]
all_snaps = all_snaps[:max_snaps]

data = {
    'height': ${SNAP_HEIGHT},
    'filename': '${FILENAME}',
    'size_bytes': ${SIZE_BYTES},
    'size_human': '${SIZE_HUMAN}',
    'sha256': '${SHA256}',
    'created': '${CREATED}',
    'snapshots': all_snaps
}

with open(os.path.join(webdir, 'bootstrap.json'), 'w') as f:
    json.dump(data, f, indent=2)
print('bootstrap.json updated')
"

# Update SHA256SUMS
echo "${SHA256}  ${FILENAME}" >> "${WEBDIR}/SHA256SUMS"

# Clean up old snapshots (keep only those in bootstrap.json)
KEEP_FILES=$(python3 -c "
import json
with open('${WEBDIR}/bootstrap.json') as f:
    snaps = json.load(f).get('snapshots', [])
for s in snaps:
    print(s['filename'])
")
for f in "${WEBDIR}"/freycoin-bootstrap-*.tar.gz; do
    BASENAME=$(basename "$f")
    if ! echo "$KEEP_FILES" | grep -qF "$BASENAME"; then
        log "Removing old snapshot: $BASENAME"
        rm -f "$f"
    fi
done

# Restart node
log "Restarting $SERVICE..."
systemctl start "$SERVICE"
sleep 5
if systemctl is-active --quiet "$SERVICE"; then
    log "Node restarted successfully"
else
    log "WARNING: Node failed to restart! Manual intervention needed."
fi

log "Bootstrap generation complete: height=$SNAP_HEIGHT"
