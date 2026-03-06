#!/bin/bash
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
CFLAGS="-O2 -Wall -Wextra -std=c11 -Icore -Itest"
CORE="core/residc.c"
PASS=0; FAIL=0
for t in test/test_*.c; do
    name=$(basename "$t" .c)
    echo "=== $name ==="
    $CC $CFLAGS -o "test/$name" "$t" $CORE && { if "./test/$name"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi; } || FAIL=$((FAIL+1))
done
echo ""; echo "$PASS suites passed, $FAIL failed"
[ $FAIL -eq 0 ]
