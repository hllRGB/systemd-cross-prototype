#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

BIN=./systemd-cross
FIX=tests/fixtures/strata
TMP=$(mktemp -d /tmp/systemd-cross-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

fail() {
        echo "FAIL: $*" >&2
        exit 1
}

pass() {
        echo "ok: $*"
}

[ -x "$BIN" ] || make >/dev/null

# ---------------------------------------------------------------- basic plan
cp tests/test.conf "$TMP/test.conf"
"$BIN" --config "$TMP/test.conf" --strata-root "$FIX" generate "$TMP/gen" >/dev/null || fail "generate"

[ -f "$TMP/gen/bedrock-beta-foo.service" ] || fail "foo unit missing"
[ -f "$TMP/gen/bedrock-beta-bar.service" ] || fail "bar unit missing (closure)"
pass "closure"

grep -q '^Requires=bedrock-beta-bar.service$' "$TMP/gen/bedrock-beta-foo.service" || fail "dependency not renamed"
grep -q '^After=network-online.target$' "$TMP/gen/bedrock-beta-foo.service" || fail "local ordering dep changed"
pass "dependency rewrite"

grep -q '^ExecStart=/bedrock/bin/strat -r beta /usr/bin/foo --daemon$' "$TMP/gen/bedrock-beta-foo.service" || fail "ExecStart not wrapped"
grep -q '^ExecStartPre=-/bedrock/bin/strat -r beta /usr/bin/foo --check$' "$TMP/gen/bedrock-beta-foo.service" || fail "ExecStart prefix handling"
pass "exec rewrite"

grep -q "^EnvironmentFile=.*$FIX/beta/etc/foo.conf$" "$TMP/gen/bedrock-beta-foo.service" || fail "EnvironmentFile not qualified"
pass "path qualification"

[ -L "$TMP/gen/multi-user.target.wants/bedrock-beta-foo.service" ] || fail "wants link missing"
[ "$(readlink "$TMP/gen/multi-user.target.wants/bedrock-beta-foo.service")" = "../bedrock-beta-foo.service" ] || fail "wants link not relative"
pass "install links"

# ---------------------------------------------------------------- masked unit
printf 'version = 1\n\n[resolve]\ninit = alpha\norder = beta\n\n[stratum beta]\nunits = masked.service\n' > "$TMP/masked.conf"
if "$BIN" --config "$TMP/masked.conf" --strata-root "$FIX" check >/dev/null 2>&1; then
        fail "masked unit accepted"
fi
pass "masked rejected"

# ---------------------------------------------------------------- alias, template
printf 'version = 1\n\n[resolve]\ninit = alpha\norder = beta\n\n[stratum beta]\nunits = alias-foo.service\n' > "$TMP/alias.conf"
"$BIN" --config "$TMP/alias.conf" --strata-root "$FIX" generate "$TMP/gen-alias" >/dev/null || fail "alias generate"
grep -q '^Description=\[beta\] Foo Daemon$' "$TMP/gen-alias/bedrock-beta-alias-foo.service" || fail "alias content not resolved"
pass "alias within stratum root"

printf 'version = 1\n\n[resolve]\ninit = alpha\norder = beta\n\n[stratum beta]\nunits = tmpl@one.service\n' > "$TMP/tmpl.conf"
"$BIN" --config "$TMP/tmpl.conf" --strata-root "$FIX" generate "$TMP/gen-tmpl" >/dev/null || fail "template generate"
grep -q '^ExecStart=/bedrock/bin/strat -r beta /usr/bin/foo %i$' "$TMP/gen-tmpl/bedrock-beta-tmpl@one.service" || fail "template fallback failed"
pass "template instance"

# ---------------------------------------------------------------- offline verify
V="$TMP/verify"
mkdir -p "$V/etc/systemd/system" "$V/usr/lib/systemd/system" "$V/bedrock/bin" "$V/usr/bin"
cp -a "$FIX/alpha/usr/lib/systemd/system/." "$V/usr/lib/systemd/system/"
cp -a "$TMP/gen/." "$V/etc/systemd/system/"
touch "$V/bedrock/bin/strat" "$V/usr/bin/foo" "$V/usr/bin/bar"
chmod +x "$V/bedrock/bin/strat" "$V/usr/bin/foo" "$V/usr/bin/bar"
systemd-analyze verify --root="$V" bedrock-beta-foo.service bedrock-beta-bar.service >/dev/null 2>&1 \
        || fail "systemd-analyze verify"
pass "offline verify"

# ---------------------------------------------------------------- setup/delete
cp tests/test.conf "$TMP/edit.conf"
"$BIN" --config "$TMP/edit.conf" --strata-root "$FIX" delete beta foo.service >/dev/null || fail "delete"
if grep -q 'units = foo.service' "$TMP/edit.conf"; then
        fail "delete did not remove entry"
fi
"$BIN" --config "$TMP/edit.conf" --strata-root "$FIX" setup beta foo.service >/dev/null || fail "setup"
grep -q 'units = foo.service' "$TMP/edit.conf" || fail "setup did not add entry"
pass "setup/delete roundtrip"

# ---------------------------------------------------------------- generator invocation
"$BIN" --config "$TMP/test.conf" --strata-root "$FIX" "$TMP/gen-direct" "$TMP/early" "$TMP/late" >/dev/null || fail "generator invocation"
[ -f "$TMP/gen-direct/bedrock-beta-foo.service" ] || fail "generator invocation produced nothing"
pass "generator invocation"

# ---------------------------------------------------------------- import
printf 'version = 1\n\n[resolve]\ninit = alpha\norder = beta\n' > "$TMP/import.conf"
"$BIN" --config "$TMP/import.conf" --strata-root "$FIX" import beta >/dev/null || fail "import"
grep -q 'units = foo.service' "$TMP/import.conf" || fail "import did not add enabled unit"
if grep -q 'units = extra.target' "$TMP/import.conf"; then
        fail "import added a target"
fi
pass "import enabled units"

echo "all tests passed"
