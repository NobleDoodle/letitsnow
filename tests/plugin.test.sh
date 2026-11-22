#!/usr/bin/env bash
# Plugin checks: one version everywhere, the scripts parse, Omarchy accepts the plugin.
# With --build, also builds and checks the package the way Install does (no sudo, no pacman).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }

v_manifest=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$HERE/manifest.json")
v_file=$(cat "$HERE/VERSION")
v_pkg=$(sed -n 's/^pkgver=//p' "$HERE/packaging/PKGBUILD")
[ "$v_manifest" = "$v_file" ] && [ "$v_file" = "$v_pkg" ] && ok "version $v_file in manifest, VERSION and PKGBUILD" ||
  bad "versions differ: manifest $v_manifest, VERSION $v_file, PKGBUILD $v_pkg"
for f in bin/letitsnow bin/letitsnow-setup bin/letitsnow-uninstall; do
  bash -n "$HERE/$f" && ok "bash -n $f" || bad "bash -n $f"
done
python3 -I -c 'import ast,sys; ast.parse(open(sys.argv[1]).read())' "$HERE/packaging/check-package" && ok "check-package parses" || bad "check-package"
if command -v omarchy > /dev/null; then
  omarchy plugin validate "$HERE" > /dev/null && ok "omarchy plugin validate" || bad "omarchy plugin validate"
fi
if [ "${1:-}" = --build ]; then
  out=$(mktemp -d "${TMPDIR:-/tmp}/letitsnow-pkg.XXXXXX")
  "$HERE/bin/letitsnow-setup" --build-only "$out" > "$out/log" 2>&1 && ok "package builds and passes check-package" ||
    { bad "package build (log: $out/log)"; tail -5 "$out/log"; }
  ls "$out"/letitsnow-*.pkg.tar.* 2> /dev/null
fi
exit $fail
