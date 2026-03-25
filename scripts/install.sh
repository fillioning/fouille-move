#!/bin/bash
set -e
MODULE_ID="fouille"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/sound_generators"

echo "Installing $MODULE_ID to $MOVE_HOST..."
scp -r "dist/$MODULE_ID" "root@$MOVE_HOST:$DEST/"
ssh root@$MOVE_HOST "chown -R ableton:users $DEST/$MODULE_ID && chmod +x $DEST/$MODULE_ID/dsp.so"
echo "Done. Verify: ssh root@$MOVE_HOST 'ls -la $DEST/$MODULE_ID/'"
