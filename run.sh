#!/usr/bin/env bash
# Launches the game via Proton with the winmm.dll override needed for
# Ultimate ASI Loader. Useful for quick iteration without a game launcher.
#
#   PROTON=/path/to/proton PREFIX=/path/to/prefix ./run.sh
#
# PROTON    the `proton` script of a Proton build
# PREFIX    Proton prefix: the folder that contains pfx/ (a plain Wine prefix
#           works too if it contains a `pfx -> .` symlink)
# GAME_DIR  optional, default: <prefix>/pfx/drive_c/Sierra/RCRacersDeluxe
set -euo pipefail

: "${PROTON:?set PROTON to the proton script of a Proton build}"
: "${PREFIX:?set PREFIX to the Proton prefix (the folder that contains pfx/)}"
GAME_DIR="${GAME_DIR:-$PREFIX/pfx/drive_c/Sierra/RCRacersDeluxe}"

export STEAM_COMPAT_DATA_PATH="$PREFIX"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/.local/share/Steam}"
export WINEDLLOVERRIDES="winmm=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"

# RCLAN_GAMESCOPE=WxH runs the game nested in a gamescope window with a WxH
# virtual screen (handy for testing next to other windows).
WRAP=()
if [ -n "${RCLAN_GAMESCOPE:-}" ]; then
    W=${RCLAN_GAMESCOPE%x*} H=${RCLAN_GAMESCOPE#*x}
    WRAP=(gamescope -w "$W" -h "$H" -W "$W" -H "$H" --)
fi

cd "$GAME_DIR"
exec "${WRAP[@]}" "$PROTON" run ./RCRacers.exe "$@"
