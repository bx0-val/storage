#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
bend_bin=${BEND_BIN:-$HOME/.bend/bin/bend}
clang_bin=${CLANG_BIN:-$HOME/.local/share/storage-toolchain/usr/bin/clang-18}
if [[ ! -x $clang_bin ]]; then clang_bin=$(command -v clang); fi
if [[ $("$bend_bin" version) != 'bend 2.0.27' ]]; then
    printf '%s\n' 'This native effect adapter is tested with Bend 2.0.27. Review its data layouts before changing compiler versions.' >&2
    exit 1
fi
mkdir -p build
"$bend_bin" PROOF.bend
"$bend_bin" main.bend -o build/storage.c
"$clang_bin" -O2 -pthread -I . -Dmain=bend_runtime_main -c build/storage.c -o build/storage.o
"$clang_bin" -O2 -pthread entry.c build/storage.o -lm -o build/storage
