#!/bin/bash
# Generate a clean bootstrap snapshot for freycoin.tech/bootstrap/
#
# Usage: ./generate-bootstrap.sh
#
# This script:
#   1. Stops the mainnet node cleanly (waits up to 5 min, then force-kills)
#   2. Creates a tar.gz of blocks/ + chainstate/ (excluding LOCK/LOG files)
#   3. Generates bootstrap.json metadata
#   4. Moves the archive to the web directory
#   5. Restarts the mainnet node
#
# Run from Toronto (138.197.130.221) as root.

set -euo pipefail

DATADIR="/var/lib/freycoin/mainnet"
WEBDIR="/var/www/freycoin.tech/bootstrap"
RPC_USER="explorer"
RPC_PASS="b4b6c32b1a868f77adb0c722186e91bf60f4a9544f0c4b48"
RPC_PORT="31469"
SERVICE="freycoind-mainnet"
TMPDIR="/tmp"

rpc() {
    freycoin-cli -rpcuser="$RPC_USER" -rpcpassword="$RPC_PASS" -rpcport="$RPC_PORT" "$@"
}

echo "=== Getting current height ==="
HEIGHT=$(rpc getblockcount)
echo "Chain height: $HEIGHT"

echo "=== Stopping $SERVICE ==="
systemctl stop "$SERVICE"

# Wait up to 5 minutes for clean shutdown
for i in $(seq 1 30); do
    if ! pgrep -f "freycoind.*mainnet" > /dev/null 2>&1; then
        echo "Node stopped cleanly after $((i * 10))s"
        break
    fi
    if [ "$i" -eq 30 ]; then
        echo "Force-killing after 300s..."
        fuser -k /usr/local/bin/freycoind 2>/dev/null || true
        sleep 3
    fi
    sleep 10
done

echo "=== Creating bootstrap archive ==="
FILENAME="freycoin-bootstrap-${HEIGHT}.tar.gz"
cd "$DATADIR"
tar czf "${TMPDIR}/${FILENAME}" \
    --exclude="*/LOCK" \
    --exclude="*/LOG" \
    --exclude="*/LOG.old" \
    blocks/ chainstate/

SIZE_BYTES=$(stat -c%s "${TMPDIR}/${FILENAME}")
SIZE_HUMAN=$(numfmt --to=iec-i --suffix=B "$SIZE_BYTES")
SHA256=$(sha256sum "${TMPDIR}/${FILENAME}" | awk '{print $1}')
CREATED=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

echo "Archive: $FILENAME ($SIZE_HUMAN, sha256=$SHA256)"

echo "=== Moving to web directory ==="
mv "${TMPDIR}/${FILENAME}" "${WEBDIR}/${FILENAME}"
ln -sf "$FILENAME" "${WEBDIR}/latest.tar.gz"

echo "=== Generating bootstrap.json ==="
# Keep last 3 snapshots in the JSON
OLD_SNAPSHOTS=$(cat "${WEBDIR}/bootstrap.json" 2>/dev/null | python3 -c "
import json, sys
try:
    data = json.load(sys.stdin)
    snaps = data.get('snapshots', [])
    # Keep only the 2 most recent old snapshots
    print(json.dumps(snaps[:2]))
except:
    print('[]')
" 2>/dev/null || echo "[]")

cat > "${WEBDIR}/bootstrap.json" <<ENDJSON
{
  "height": ${HEIGHT},
  "filename": "${FILENAME}",
  "size_bytes": ${SIZE_BYTES},
  "size_human": "${SIZE_HUMAN}",
  "sha256": "${SHA256}",
  "created": "${CREATED}",
  "snapshots": $(python3 -c "
import json
new = {'height': ${HEIGHT}, 'filename': '${FILENAME}', 'size_bytes': ${SIZE_BYTES}, 'size_human': '${SIZE_HUMAN}', 'sha256': '${SHA256}'}
old = json.loads('${OLD_SNAPSHOTS}')
all_snaps = [new] + old[:2]
print(json.dumps(all_snaps))
")
}
ENDJSON

# SHA256SUMS file
echo "${SHA256}  ${FILENAME}" >> "${WEBDIR}/SHA256SUMS"

echo "=== Restarting $SERVICE ==="
systemctl start "$SERVICE"
sleep 3
systemctl is-active "$SERVICE" && echo "Node restarted OK" || echo "WARNING: Node failed to start!"

echo "=== Done ==="
echo "Bootstrap: ${WEBDIR}/${FILENAME}"
echo "Height: $HEIGHT | Size: $SIZE_HUMAN | SHA256: $SHA256"
