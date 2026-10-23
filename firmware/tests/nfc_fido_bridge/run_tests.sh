#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
bridge_dir="$test_dir/../../application/src/urat_esp"
build_dir="$test_dir/build"
mkdir -p "$build_dir"
"${CC:-cc}" -std=c11 -O0 -fno-builtin -Wall -Wextra -Werror \
    -I "$test_dir/stubs" -I "$bridge_dir" \
    "$bridge_dir/nfc_fido_bridge.c" "$test_dir/test_env.c" \
    "$test_dir/test_bridge.c" "$test_dir/freestanding.c" \
    -o "$build_dir/nfc_fido_bridge_test"
"$build_dir/nfc_fido_bridge_test"
printf '%s\n' 'nfc_fido_bridge_test: all behavior checks passed'
