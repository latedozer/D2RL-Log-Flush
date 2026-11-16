#!/bin/bash
# Puts the built plugin into the game's global plugin folder (or takes it out again).
#
#   ./install.sh          install build/d2rl-background-log-flush.dll
#   ./install.sh remove   take the plugin out (its settings stay)
#
# Refuses while the game is running. GAME=/path/to/Diablo\ II\ Resurrected overrides the game folder.
set -e
cd "$(dirname "$0")"
GAME="${GAME:-$HOME/FI/SteamLibrary/steamapps/common/Diablo II Resurrected}"
DEST="$GAME/d2rloader/plugins"
[ -d "$DEST" ] || { echo "plugin folder not found: $DEST"; exit 1; }
if ps -eo args | grep -q "[D]2RLoader[.]exe"; then
	echo "The game is running. Close it, then run this again."
	exit 1
fi
if [ "${1:-}" = "remove" ]; then
	rm -f "$DEST/d2rl-background-log-flush.dll"
	echo "removed. Settings (config/background-log-flush.toml) were left in $GAME/d2rloader"
	exit 0
fi
[ -f build/d2rl-background-log-flush.dll ] || { echo "build it first: ./build.sh"; exit 1; }
cp build/d2rl-background-log-flush.dll "$DEST/d2rl-background-log-flush.dll.installing"
mv "$DEST/d2rl-background-log-flush.dll.installing" "$DEST/d2rl-background-log-flush.dll"
ls -la "$DEST/d2rl-background-log-flush.dll"
echo "installed. Start the game; type logflush in the console (Ctrl + \`) to see it work."
