#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
./build.sh
python3 test_storage.py
clang_bin=${CLANG_BIN:-$HOME/.local/share/storage-toolchain/usr/bin/clang-18}
if [[ ! -x $clang_bin ]]; then clang_bin=$(command -v clang); fi
"$clang_bin" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -I . \
    -Dmain=bend_runtime_main -c build/storage.c -o build/storage-asan.o
"$clang_bin" -O1 -g -fsanitize=address,undefined -pthread \
    entry.c build/storage-asan.o -lm -o build/storage-asan
STORAGE_BIN="$PWD/build/storage-asan" python3 test_storage.py
