#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
fail() { printf 'build.sh: %s\n' "$*" >&2; exit 1; }
install_requested=false
case ${1:-} in
    '') ;;
    --install) install_requested=true ;;
    -h|--help)
        printf '%s\n' 'Usage: ./build.sh [--install]' 'Build storage; --install also runs tests and installs to ${INSTALL_DIR:-$HOME/.local/bin}.'
        exit 0 ;;
    *) fail "Unknown argument: $1 (use --help)" ;;
esac
[[ $# -le 1 ]] || fail 'Too many arguments (use --help)'

# Keep a project-local pinned compiler separate from the user's current Bend.
bend_bin=${BEND_BIN:-}
if [[ -z $bend_bin ]]; then
    for candidate in "$PWD/build/toolchain/bend/bin/bend" "$HOME/.bend/bin/bend" bend; do
        if command -v "$candidate" >/dev/null 2>&1; then bend_bin=$candidate; break; fi
    done
fi
[[ -n $bend_bin ]] && command -v "$bend_bin" >/dev/null 2>&1 || fail 'Bend not found. Install Bend 2.0.27 or set BEND_BIN to its executable.'
bend_version=$("$bend_bin" version) || fail "Cannot run Bend: $bend_bin"
if [[ $bend_version != 'bend 2.0.27' ]]; then
    printf 'build.sh: Bend 2.0.27 required; found %s at %s. Set BEND_BIN to a 2.0.27 executable.\n' "$bend_version" "$bend_bin" >&2
    printf '%s\n' 'The native effect adapter depends on compiler-specific data layouts; do not bypass this check.' >&2
    exit 1
fi
clang_bin=${CLANG_BIN:-}
if [[ -z $clang_bin ]]; then
    for candidate in "$PWD/build/toolchain/clang/usr/bin/clang-18" "$HOME/.local/share/storage-toolchain/usr/bin/clang-18" clang-18 clang; do
        if command -v "$candidate" >/dev/null 2>&1; then clang_bin=$candidate; break; fi
    done
fi
[[ -n $clang_bin ]] && command -v "$clang_bin" >/dev/null 2>&1 || fail 'Clang not found. On Ubuntu, install clang-18 (sudo apt install clang-18), or set CLANG_BIN to a compatible Clang executable.'
"$clang_bin" --version >/dev/null || fail "Cannot run Clang: $clang_bin"
if $install_requested; then
    command -v python3 >/dev/null 2>&1 || fail 'python3 is required to test before installing.'
fi
mkdir -p build
"$bend_bin" PROOF.bend
"$bend_bin" main.bend -o build/storage.c
"$clang_bin" -O2 -pthread -I . -Dmain=bend_runtime_main -c build/storage.c -o build/storage.o
"$clang_bin" -O2 -pthread entry.c build/storage.o -lm -o build/storage
printf '%s\n' 'Built build/storage'
if $install_requested; then
    STORAGE_BIN="$PWD/build/storage" python3 test_storage.py
    install_dir=${INSTALL_DIR:-$HOME/.local/bin}
    mkdir -p -- "$install_dir"
    staged_binary=$(mktemp "$install_dir/.storage.XXXXXX")
    trap 'rm -f -- "$staged_binary"' EXIT
    install -m 755 build/storage "$staged_binary"
    mv -f -- "$staged_binary" "$install_dir/storage"
    printf 'Installed %s/storage\n' "$install_dir"
else
    printf '%s\n' 'Run ./build.sh --install to test and install to ~/.local/bin.'
fi
