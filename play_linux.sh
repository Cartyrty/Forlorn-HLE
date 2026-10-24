#!/usr/bin/env bash
# Starts Forlorn on Linux. Saves go to the "sandbox" folder next to this file; the log of the last run to
# forlorn-log.txt. Libraries unpacked by source/compile_linux.sh --local-deps are used when present.
cd "$(dirname "${BASH_SOURCE[0]}")"
deps="${XDG_DATA_HOME:-$HOME/.local/share}/forlorn-hle/deps/root/usr/lib/x86_64-linux-gnu"
if [ -d "$deps" ]; then
    export LD_LIBRARY_PATH="$(find "$deps" -type d | tr '\n' ':')${LD_LIBRARY_PATH:-}"
fi
if [ ! -x ./ForlornHLE ]; then
    echo "ForlornHLE is not built yet: run source/compile_linux.sh first."
    exit 1
fi
export HLE_QUIET="${HLE_QUIET:-bridge,file,kvc,touch}"
exec ./ForlornHLE > forlorn-log.txt 2>&1
