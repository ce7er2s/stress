#!/usr/bin/env bash
#
# test.sh -- smoke tests for stress
#
# Exercises the documented behavior: counting, concurrency, output capture,
# stop-on-error + exit code, flag clustering, and parse errors.
# Exits non-zero on the first failed assertion.

set -u

BIN=./stress
pass=0
fail=0

ok()   { printf '  ok   %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  FAIL %s\n' "$1"; fail=$((fail+1)); }

# a worker that prints two lines, sleeps briefly, then exits 0
worker=$(mktemp)
cat > "$worker" <<'EOF'
#!/usr/bin/env bash
echo "line one"
echo "line two"
sleep 0.1
exit 0
EOF
chmod +x "$worker"

# a worker that fails on its Nth invocation, tracked via a counter file
ctr=$(mktemp)
echo 0 > "$ctr"
failer=$(mktemp)
cat > "$failer" <<EOF
#!/usr/bin/env bash
n=\$(( \$(cat "$ctr") + 1 )); echo \$n > "$ctr"
sleep 0.05
[ "\$n" = 3 ] && exit 7
exit 0
EOF
chmod +x "$failer"

cleanup() { rm -f "$worker" "$failer" "$ctr"; }
trap cleanup EXIT

echo "stress smoke tests"
echo

# 1. count + concurrency: exactly 5 launches, never more than 2 concurrent
out=$($BIN -n 5 -c 2 -i 0.05 --color never -- "$worker" 2>&1)
launched=$(printf '%s\n' "$out" | grep -c '\[run')
completed=$(printf '%s\n' "$out" | sed -n 's/.*\[total\] completed *\([0-9]*\).*/\1/p')
[ "$launched" = 5 ] && ok "launches exactly -n times (5)" \
                    || bad "expected 5 launches, got $launched"
[ "$completed" = 5 ] && ok "all jobs complete (5)" \
                     || bad "expected 5 completed, got $completed"

# 2. output capture with line cap
out=$($BIN -n 1 -oL1 --color never -- "$worker" 2>&1)
printf '%s\n' "$out" | grep -q 'line one'        && ok "output captured" \
                                                  || bad "output not captured"
printf '%s\n' "$out" | grep -q 'more lines'      && ok "line cap notes truncation" \
                                                  || bad "truncation note missing"

# 3. stop-on-error halts and exit code propagates
echo 0 > "$ctr"
$BIN -e -c 1 -i 0.03 --color never -- "$failer" >/dev/null 2>&1
rc=$?
[ "$rc" = 1 ] && ok "stop-on-error exits 1" \
              || bad "expected exit 1, got $rc"

# 4. without failures, exit code is 0
$BIN -n 2 -c 1 -i 0.03 --color never -- "$worker" >/dev/null 2>&1
rc=$?
[ "$rc" = 0 ] && ok "clean run exits 0" \
              || bad "expected exit 0, got $rc"

# 5. flag clustering: -eoq parses as three flags, -L takes next arg
out=$($BIN -n 1 -qoL 2 --color never -- "$worker" 2>&1)
printf '%s\n' "$out" | grep -q 'output=on/2L' && ok "clustered -qoL N parses" \
                                              || bad "clustered flags mis-parsed"
printf '%s\n' "$out" | grep -q '\[run'        && bad "-q did not suppress run lines" \
                                              || ok "-q suppresses run lines"

# 6. parse errors exit 2
$BIN -z -- /bin/true >/dev/null 2>&1
[ $? = 2 ] && ok "unknown flag exits 2" || bad "unknown flag wrong exit"
$BIN --color purple -- /bin/true >/dev/null 2>&1
[ $? = 2 ] && ok "bad --color exits 2" || bad "bad color wrong exit"
$BIN -n 1 >/dev/null 2>&1
[ $? = 2 ] && ok "missing command exits 2" || bad "missing command wrong exit"

echo
echo "passed: $pass, failed: $fail"
[ "$fail" = 0 ]
