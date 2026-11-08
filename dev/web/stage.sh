#!/bin/bash
# Copy the fake programmers and the test runner into a built web/dist, under
# _t/, and print what to paste into the browser console. Remove _t/ before
# deploying that dist anywhere.
#
# Usage: dev/web/stage.sh [DIST]    (default: web/dist)
set -eu
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
DIST=${1:-$ROOT/web/dist}
[ -f "$DIST/index.html" ] || { echo "no web build at $DIST (run make -C web)"; exit 1; }
mkdir -p "$DIST/_t"
cp "$ROOT"/dev/web/*.js "$DIST/_t/"
cat <<'EOF'
Staged in _t/. Serve the dist (for example: python3 -m http.server 8347
--bind 127.0.0.1 from inside it), open http://127.0.0.1:8347/ in Chrome
and run in the console:

  eval(await (await fetch('/_t/run_programmer_test.js')).text());
  await runProgrammerTest('/_t/fake_ch341a_webusb.js', '__ch341');

Reload, then the same with '/_t/fake_ch347_webusb.js', '__ch347'.
EOF
